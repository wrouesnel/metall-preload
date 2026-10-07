// metall-preload: an LD_PRELOAD shim that serves the heap of the Determinate
// Systems nix binary (malloc family, C++ operator new/delete, and the Boehm GC
// allocation API) from an LLNL Metall datastore in an ephemeral directory
// under /tmp. The Boehm collector is disabled: nothing is ever collected, the
// heap simply lives in sparse, file-backed shared mappings instead of RAM.
//
// Design notes (see README.md for the user-facing summary):
//
// * Only process-wide constant-initialised POD state is touched on the hot
//   path, so allocations that arrive before our constructor (or from
//   inside dlsym) are safe; they go to the next allocator (mimalloc in nix).
// * Pointer ownership is decided purely by address range, so pointers from
//   the next allocator are always handed back to it.
// * The whole segment is created and mapped at startup (sparse files), so
//   Metall never has to create or extend files later. Its extension failure
//   path unmaps the entire heap, and we never want to reach it.
// * The heap is MAP_SHARED, which a forked child would share with its parent.
//   pthread_atfork handlers (and a clone() interposer, since nix uses raw
//   clone() for sandboxed builds) give each child its own copy of the heap
//   in an unlinked file.

#include <metall/metall.hpp>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sched.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

namespace {

// ---------------------------------------------------------------------------
// Metall types and private member access
// ---------------------------------------------------------------------------

constexpr std::size_t kChunkSize = 1ULL << 21;
constexpr std::size_t kBlockSize = METALL_SEGMENT_BLOCK_SIZE;
constexpr std::size_t kMaxSources = 1024;
constexpr std::size_t kDefaultCapacity = 256ULL << 30;
constexpr std::size_t kDefaultMinFree = 1ULL << 30;
// Headroom kept free in the segment so that concurrent allocations can never
// push Metall into its (fatal) segment extension path.
constexpr std::size_t kSegmentSlack = 64ULL << 20;
// Re-check free disk space after this many bytes have been allocated.
constexpr std::size_t kDiskCheckInterval = 64ULL << 20;
// Alignments above this are not guaranteed by Metall (it only aligns relative
// to the page-aligned segment start), so they go to the next allocator.
constexpr std::size_t kMaxMetallAlign = 4096;

using kernel_t = metall::manager::manager_kernel_type;
using sstorage_t = metall::kernel::segment_storage;
using salloc_t =
    metall::kernel::segment_allocator<uint32_t, std::size_t, std::ptrdiff_t,
                                      kChunkSize, METALL_MAX_CAPACITY,
                                      sstorage_t>;
using cdir_t =
    metall::kernel::chunk_directory<uint32_t, kChunkSize, METALL_MAX_CAPACITY>;
using binmgr_t =
    metall::kernel::bin_number_manager<kChunkSize, METALL_MAX_CAPACITY>;

// Explicit instantiation may name private members; this is the standard
// legal way to take a pointer-to-member of one.
template <typename Tag, typename Tag::type M>
struct Rob {
  friend typename Tag::type get(Tag) { return M; }
};
struct KernelAlloc {
  using type = salloc_t kernel_t::*;
  friend type get(KernelAlloc);
};
struct KernelStorage {
  using type = sstorage_t kernel_t::*;
  friend type get(KernelStorage);
};
struct AllocDir {
  using type = cdir_t salloc_t::*;
  friend type get(AllocDir);
};
template struct Rob<KernelAlloc, &kernel_t::m_segment_memory_allocator>;
template struct Rob<KernelStorage, &kernel_t::m_segment_storage>;
template struct Rob<AllocDir, &salloc_t::m_chunk_directory>;

// ---------------------------------------------------------------------------
// Global state. Everything here is constant-initialised.
// ---------------------------------------------------------------------------

struct Source {
  int fd;
  std::size_t off;  // offset of this file within the segment
  std::size_t len;
};

enum ForkMode { kForkNone, kForkSnapshot, kForkPrivate };

// Set once initialisation has fully succeeded.
std::atomic<bool> g_active{false};
// Address range served by Metall; g_seg_len == 0 means "owns nothing".
std::uintptr_t g_seg_begin = 0;
std::uintptr_t g_seg_len = 0;
// Bytes of the segment backed by mapped files.
std::size_t g_mapped = 0;

alignas(kernel_t) unsigned char g_kernel_storage[sizeof(kernel_t)];
kernel_t *g_kernel = nullptr;
const cdir_t *g_chunk_dir = nullptr;

// New allocations are refused while the disk is nearly full, and in children
// whose heap could only be remapped privately.
std::atomic<bool> g_disk_low{false};
bool g_no_new_allocs = false;
std::atomic<std::size_t> g_since_disk_check{0};
std::size_t g_min_free = kDefaultMinFree;

// Files backing the segment, used as the source of fork snapshots.
Source g_sources[kMaxSources];
std::size_t g_num_sources = 0;
// Other descriptors to drop in fork children (Metall lockfile, owner.lock).
int g_other_fds[8];
std::size_t g_num_other_fds = 0;
// Whether a fork child needs its own copy of the heap. False once the heap is
// a private mapping, which the kernel copies-on-write by itself.
bool g_need_snapshot = true;

bool g_verbose = false;
pid_t g_owner_pid = 0;  // Only this process may delete g_dir.
char g_root[512] = "/tmp";
char g_dir[600] = "";

struct PendingFork {
  ForkMode mode;
  int fd;
  bool active;
};
PendingFork g_fork = {kForkNone, -1, false};

// Non-zero while this thread is inside Metall, initialisation or a fork
// handler: any allocation it makes goes to the next allocator.
__thread int t_busy __attribute__((tls_model("initial-exec"))) = 0;

// ---------------------------------------------------------------------------
// Logging (async-signal-safe enough for fork handlers: no allocation)
// ---------------------------------------------------------------------------

__attribute__((format(printf, 1, 2))) void log_msg(const char *fmt, ...) {
  char buf[1024];
  int n = snprintf(buf, sizeof(buf), "metall-preload[%d]: ", (int)getpid());
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
  va_end(ap);
  if (m < 0) m = 0;
  std::size_t len = std::min<std::size_t>(n + m, sizeof(buf) - 2);
  buf[len++] = '\n';
  ssize_t r = write(STDERR_FILENO, buf, len);
  (void)r;
}

#define VLOG(...)                     \
  do {                                \
    if (g_verbose) log_msg(__VA_ARGS__); \
  } while (0)

// ---------------------------------------------------------------------------
// The next allocator in the chain, and a bootstrap arena used while dlsym is
// resolving it (dlsym itself may call malloc/calloc).
// ---------------------------------------------------------------------------

struct Next {
  void *(*malloc)(std::size_t);
  void (*free)(void *);
  void *(*calloc)(std::size_t, std::size_t);
  void *(*realloc)(void *, std::size_t);
  int (*posix_memalign)(void **, std::size_t, std::size_t);
  void *(*aligned_alloc)(std::size_t, std::size_t);
  void *(*memalign)(std::size_t, std::size_t);
  std::size_t (*malloc_usable_size)(void *);
  int (*clone)(int (*)(void *), void *, int, void *, ...);
};
Next g_next = {};
std::atomic<int> g_resolve_state{0};  // 0: not yet, 1: resolving, 2: done

alignas(64) unsigned char g_boot[256 << 10];
std::atomic<std::size_t> g_boot_used{0};

bool is_boot(const void *p) {
  return p >= g_boot && p < g_boot + sizeof(g_boot);
}

void *boot_alloc(std::size_t n) {
  // 16-byte header holding the size, then the 16-byte aligned payload.
  std::size_t need = 16 + ((n + 15) & ~std::size_t(15));
  std::size_t off = g_boot_used.fetch_add(need);
  if (off + need > sizeof(g_boot)) return nullptr;
  *reinterpret_cast<std::size_t *>(g_boot + off) = n;
  return g_boot + off + 16;  // zeroed: the arena is never reused
}

std::size_t boot_size(const void *p) {
  return *reinterpret_cast<const std::size_t *>(
      static_cast<const unsigned char *>(p) - 16);
}

template <typename T>
void resolve_one(T &slot, const char *name) {
  slot = reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}

void resolve_next() {
  int expected = 0;
  if (!g_resolve_state.compare_exchange_strong(expected, 1)) {
    // Another thread is resolving; it will not take long.
    while (g_resolve_state.load() != 2) sched_yield();
    return;
  }
  Next n = {};
  resolve_one(n.malloc, "malloc");
  resolve_one(n.free, "free");
  resolve_one(n.calloc, "calloc");
  resolve_one(n.realloc, "realloc");
  resolve_one(n.posix_memalign, "posix_memalign");
  resolve_one(n.aligned_alloc, "aligned_alloc");
  resolve_one(n.memalign, "memalign");
  resolve_one(n.malloc_usable_size, "malloc_usable_size");
  resolve_one(n.clone, "clone");
  g_next = n;
  g_resolve_state.store(2);
}

inline bool next_ready() {
  if (__builtin_expect(g_resolve_state.load(std::memory_order_acquire) == 2, 1))
    return true;
  if (g_resolve_state.load() == 1) return false;  // we are inside dlsym
  resolve_next();
  return g_resolve_state.load() == 2;
}

void *next_malloc(std::size_t n) {
  if (!next_ready()) return boot_alloc(n);
  return g_next.malloc(n);
}

void *next_calloc(std::size_t n, std::size_t m) {
  if (!next_ready()) {
    std::size_t total;
    if (__builtin_mul_overflow(n, m, &total)) return nullptr;
    return boot_alloc(total);
  }
  return g_next.calloc(n, m);
}

void next_free(void *p) {
  if (is_boot(p)) return;
  if (!next_ready()) return;  // leak rather than recurse during bootstrap
  g_next.free(p);
}

void *next_memalign(std::size_t align, std::size_t n) {
  if (!next_ready()) return align <= 16 ? boot_alloc(n) : nullptr;
  return g_next.memalign(align, n);
}

// ---------------------------------------------------------------------------
// A reader gate held around every Metall call. fork() takes it exclusively
// so that no thread is inside Metall (holding its internal mutexes) when the
// address space is copied. Readers are spread over cache-line shards.
// ---------------------------------------------------------------------------

constexpr int kShards = 64;
struct alignas(64) Shard {
  std::atomic<long> n;
};
Shard g_shards[kShards];
std::atomic<int> g_writer{0};
std::atomic<unsigned> g_shard_seq{0};
__thread int t_shard __attribute__((tls_model("initial-exec"))) = -1;

inline Shard &my_shard() {
  if (__builtin_expect(t_shard < 0, 0))
    t_shard = int(g_shard_seq.fetch_add(1, std::memory_order_relaxed) %
                  kShards);
  return g_shards[t_shard];
}

inline void gate_enter() {
  Shard &s = my_shard();
  for (;;) {
    s.n.fetch_add(1, std::memory_order_seq_cst);
    if (__builtin_expect(!g_writer.load(std::memory_order_seq_cst), 1)) return;
    s.n.fetch_sub(1, std::memory_order_seq_cst);
    while (g_writer.load(std::memory_order_relaxed)) sched_yield();
  }
}

inline void gate_exit() { my_shard().n.fetch_sub(1, std::memory_order_release); }

void gate_lock() {
  while (g_writer.exchange(1, std::memory_order_seq_cst)) sched_yield();
  for (auto &s : g_shards)
    while (s.n.load(std::memory_order_seq_cst) != 0) sched_yield();
}

void gate_unlock() { g_writer.store(0, std::memory_order_release); }

// ---------------------------------------------------------------------------
// Metall operations
// ---------------------------------------------------------------------------

inline bool owns(const void *p) {
  return reinterpret_cast<std::uintptr_t>(p) - g_seg_begin < g_seg_len;
}

void check_disk() {
  if (g_num_sources == 0) return;
  struct statvfs sv;
  if (fstatvfs(g_sources[0].fd, &sv) != 0) return;
  const unsigned long long avail =
      (unsigned long long)sv.f_bavail * sv.f_frsize;
  const bool low = avail < g_min_free;
  if (low != g_disk_low.load(std::memory_order_relaxed)) {
    log_msg("%s: %llu MiB free on %s", low
                ? "disk nearly full, new allocations bypass Metall"
                : "disk space recovered, resuming Metall allocations",
            avail >> 20, g_root);
    g_disk_low.store(low, std::memory_order_relaxed);
  }
}

inline void account(std::size_t n) {
  if (g_since_disk_check.fetch_add(n, std::memory_order_relaxed) + n <
      kDiskCheckInterval)
    return;
  g_since_disk_check.store(0, std::memory_order_relaxed);
  check_disk();
}

// Returns nullptr whenever the caller should fall back to the next allocator.
// Metall memory is not zeroed.
void *m_alloc(std::size_t n, std::size_t align) {
  if (!g_active.load(std::memory_order_acquire) || t_busy) return nullptr;
  if (n > g_mapped) return nullptr;
  // 16-byte granularity keeps every object 16-byte aligned (malloc's
  // guarantee); every Metall size class that is a multiple of 16 exists.
  std::size_t sz = n < 16 ? 16 : (n + 15) & ~std::size_t(15);
  if (align > 16) {
    if (align > kMaxMetallAlign) return nullptr;
    sz = (sz + align - 1) & ~(align - 1);
  }
  account(sz);
  if (g_no_new_allocs || g_disk_low.load(std::memory_order_relaxed))
    return nullptr;
  // Never let Metall run out of mapped segment (see file header). The chunk
  // directory size is read racily, the slack covers concurrent allocations.
  if (g_chunk_dir->size() * kChunkSize + sz + kSegmentSlack > g_mapped)
    return nullptr;

  ++t_busy;
  gate_enter();
  void *p = nullptr;
  try {
    p = align > 16 ? g_kernel->allocate_aligned(sz, align)
                   : g_kernel->allocate(sz);
  } catch (...) {
    p = nullptr;
  }
  gate_exit();
  --t_busy;
  return p;
}

void m_free(void *p) {
  ++t_busy;
  gate_enter();
  try {
    g_kernel->deallocate(p);
  } catch (...) {
  }
  gate_exit();
  --t_busy;
}

std::size_t m_usable(const void *p) {
  const std::size_t chunk =
      (reinterpret_cast<std::uintptr_t>(p) - g_seg_begin) / kChunkSize;
  return binmgr_t::to_object_size(g_chunk_dir->bin_no(uint32_t(chunk)));
}

inline void *alloc_any(std::size_t n) {
  void *p = m_alloc(n, 0);
  return p ? p : next_malloc(n);
}

inline void free_any(void *p) {
  if (!p) return;
  if (owns(p))
    m_free(p);
  else
    next_free(p);
}

void *realloc_any(void *p, std::size_t n) {
  if (!p) return alloc_any(n);
  if (!owns(p) && !is_boot(p)) {
    if (!next_ready()) return nullptr;
    return g_next.realloc(p, n);
  }
  if (n == 0) {  // glibc semantics: free and return NULL
    free_any(p);
    return nullptr;
  }
  const std::size_t old = is_boot(p) ? boot_size(p) : m_usable(p);
  if (!is_boot(p) && n <= old && (old <= 4096 || n >= old / 2)) return p;
  void *q = alloc_any(n);
  if (!q) return nullptr;  // p is left untouched, as realloc requires
  std::memcpy(q, p, std::min(old, n));
  free_any(p);
  return q;
}

bool valid_posix_align(std::size_t a) {
  return a >= sizeof(void *) && (a & (a - 1)) == 0;
}

void *memalign_any(std::size_t align, std::size_t n) {
  if (align <= 16) return alloc_any(n);
  if ((align & (align - 1)) != 0) {
    errno = EINVAL;
    return nullptr;
  }
  void *p = m_alloc(n, align);
  return p ? p : next_memalign(align, n);
}

// ---------------------------------------------------------------------------
// Filesystem helpers (used during init, exit and by the reaper)
// ---------------------------------------------------------------------------

void remove_tree_at(int parent_fd, const char *name) {
  int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                       O_CLOEXEC);
  if (fd >= 0) {
    DIR *d = fdopendir(fd);
    if (d) {
      while (struct dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_type == DT_DIR)
          remove_tree_at(dirfd(d), e->d_name);
        else if (unlinkat(dirfd(d), e->d_name, 0) != 0 && errno == EISDIR)
          remove_tree_at(dirfd(d), e->d_name);
      }
      closedir(d);
    } else {
      close(fd);
    }
  }
  unlinkat(parent_fd, name, AT_REMOVEDIR);
}

void remove_tree(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash) return;
  char parent[sizeof(g_dir)];
  std::size_t plen = std::max<std::size_t>(slash - path, 1);
  if (plen >= sizeof(parent)) return;
  memcpy(parent, path, plen);
  parent[plen] = '\0';
  int pfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (pfd < 0) return;
  remove_tree_at(pfd, slash + 1);
  close(pfd);
}

// Removes heap directories left behind by processes that died without
// running their exit handlers.
void reap_stale_dirs() {
  int rfd = open(g_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (rfd < 0) return;
  DIR *d = fdopendir(rfd);
  if (!d) {
    close(rfd);
    return;
  }
  const time_t now = time(nullptr);
  const char *own = strrchr(g_dir, '/');
  own = own ? own + 1 : "";
  while (struct dirent *e = readdir(d)) {
    if (strncmp(e->d_name, "metall-preload-", 15) != 0) continue;
    if (!strcmp(e->d_name, own)) continue;
    struct stat st;
    if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid()) continue;
    // The name carries the owner's pid; a dead owner means a stale directory
    // (nix SIGKILLs its build hooks, so this is common).
    const long pid = strtol(e->d_name + 15, nullptr, 10);
    bool stale = pid > 0 && kill(pid_t(pid), 0) != 0 && errno == ESRCH;
    // Otherwise a live owner holds owner.lock. Young directories may belong
    // to a process that has not taken the lock yet.
    if (!stale && now - st.st_mtime >= 60) {
      char lock[600];
      snprintf(lock, sizeof(lock), "%s/owner.lock", e->d_name);
      int lfd = openat(dirfd(d), lock, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      stale = true;
      if (lfd >= 0) {
        stale = flock(lfd, LOCK_EX | LOCK_NB) == 0;
        close(lfd);
      }
    }
    if (stale) {
      VLOG("removing stale heap directory %s/%s", g_root, e->d_name);
      remove_tree_at(dirfd(d), e->d_name);
    }
  }
  closedir(d);
}

// ---------------------------------------------------------------------------
// fork() / clone() handling
// ---------------------------------------------------------------------------

int open_snapshot_file() {
  int fd = open(g_root, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
  if (fd >= 0) return fd;
  char tmpl[sizeof(g_root) + 32];
  snprintf(tmpl, sizeof(tmpl), "%s/metall-preload-fork-XXXXXX", g_root);
  fd = mkostemp(tmpl, O_CLOEXEC);
  if (fd >= 0) unlink(tmpl);
  return fd;
}

bool write_from_memory(int dst, std::size_t seg_off, std::size_t len) {
  const char *src = reinterpret_cast<const char *>(g_seg_begin) + seg_off;
  while (len > 0) {
    ssize_t w = pwrite(dst, src, len, off_t(seg_off));
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      return false;
    }
    src += w;
    seg_off += std::size_t(w);
    len -= std::size_t(w);
  }
  return true;
}

// Copies the data extents of [0, used) of the segment into dst.
bool copy_segment(int dst, std::size_t used) {
  // Reflink the whole file first where the filesystem supports it.
  bool reflinked = true;
  for (std::size_t i = 0; i < g_num_sources && reflinked; ++i) {
    const Source &s = g_sources[i];
    if (s.off >= used) break;
    struct file_clone_range r = {};
    r.src_fd = s.fd;
    r.src_offset = 0;
    r.src_length = 0;  // to end of file
    r.dest_offset = s.off;
    reflinked = ioctl(dst, FICLONERANGE, &r) == 0;
  }
  if (reflinked) return true;

  bool use_cfr = true;
  for (std::size_t i = 0; i < g_num_sources; ++i) {
    const Source &s = g_sources[i];
    if (s.off >= used) break;
    const off_t limit = off_t(std::min(s.len, used - s.off));
    off_t pos = 0;
    while (pos < limit) {
      off_t data = lseek(s.fd, pos, SEEK_DATA);
      if (data < 0) {
        if (errno == ENXIO) break;  // no more data in this file
        data = pos;                 // SEEK_DATA unsupported: copy everything
      }
      if (data >= limit) break;
      off_t hole = lseek(s.fd, data, SEEK_HOLE);
      if (hole < 0 || hole > limit) hole = limit;
      loff_t in = data, out = off_t(s.off) + data;
      while (in < hole) {
        ssize_t c = -1;
        if (use_cfr) c = copy_file_range(s.fd, &in, dst, &out, hole - in, 0);
        if (c > 0) continue;
        if (c < 0 && errno == EINTR) continue;
        if (c == 0 || errno == EXDEV || errno == ENOSYS || errno == EINVAL ||
            errno == EOPNOTSUPP || !use_cfr) {
          use_cfr = false;
          if (!write_from_memory(dst, s.off + std::size_t(in),
                                 std::size_t(hole - in)))
            return false;
          in = hole;
          continue;
        }
        return false;
      }
      pos = hole;
    }
  }
  return true;
}

void fork_prepare() {
  ++t_busy;
  g_fork = {kForkNone, -1, false};
  if (!g_active.load(std::memory_order_acquire)) return;
  gate_lock();
  g_fork.active = true;
  if (!g_need_snapshot) return;  // private heap: the kernel copies-on-write

  const std::size_t used =
      std::min(g_chunk_dir->size() * kChunkSize, g_mapped);
  unsigned long long allocated = 0;
  for (std::size_t i = 0; i < g_num_sources; ++i) {
    struct stat st;
    if (fstat(g_sources[i].fd, &st) == 0)
      allocated += (unsigned long long)st.st_blocks * 512;
  }
  struct statvfs sv;
  if (fstatvfs(g_sources[0].fd, &sv) == 0 &&
      (unsigned long long)sv.f_bavail * sv.f_frsize < allocated + g_min_free) {
    log_msg("not enough disk space to snapshot the heap for a child "
            "(%llu MiB needed); the child gets a private mapping",
            allocated >> 20);
    g_fork.mode = kForkPrivate;
    return;
  }

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int fd = open_snapshot_file();
  if (fd < 0 || ftruncate(fd, off_t(g_mapped)) != 0 || !copy_segment(fd, used)) {
    log_msg("failed to snapshot the heap for a child (%s); the child gets a "
            "private mapping", strerror(errno));
    if (fd >= 0) close(fd);
    g_fork.mode = kForkPrivate;
    return;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  VLOG("snapshotted the heap for a child (%zu MiB span, %llu MiB allocated) "
       "in %ld ms",
       used >> 20, allocated >> 20,
       (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
  g_fork.mode = kForkSnapshot;
  g_fork.fd = fd;
}

void fork_parent_impl(bool close_fd) {
  if (g_fork.active) {
    if (g_fork.fd >= 0 && close_fd) close(g_fork.fd);
    gate_unlock();
  }
  g_fork = {kForkNone, -1, false};
  --t_busy;
}

void fork_parent() { fork_parent_impl(true); }

// Runs in the child with only this thread alive; syscalls only.
void fork_child_impl(bool own_fds) {
  if (g_fork.active) {
    char *seg = reinterpret_cast<char *>(g_seg_begin);
    bool ok = false;
    if (g_fork.mode == kForkSnapshot) {
      ok = mmap(seg, g_mapped, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                g_fork.fd, 0) == seg;
      if (!ok) log_msg("failed to map the heap snapshot: %s", strerror(errno));
    }
    if (!ok && g_need_snapshot) {
      // Private copy-on-write view of the files. Pages the child has not yet
      // touched still follow the parent's writes, so stop handing out new
      // memory from this heap in the child.
      ok = true;
      for (std::size_t i = 0; i < g_num_sources; ++i) {
        const Source &s = g_sources[i];
        ok &= mmap(seg + s.off, s.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_FIXED, s.fd, 0) == seg + s.off;
      }
      if (!ok) {
        log_msg("failed to remap the heap privately: %s", strerror(errno));
        abort();
      }
      g_no_new_allocs = true;
      g_need_snapshot = false;
    }

    if (own_fds) {
      for (std::size_t i = 0; i < g_num_other_fds; ++i) close(g_other_fds[i]);
      if (g_fork.mode == kForkSnapshot && ok) {
        for (std::size_t i = 0; i < g_num_sources; ++i)
          close(g_sources[i].fd);
      }
    }
    g_num_other_fds = 0;
    if (g_fork.mode == kForkSnapshot && ok) {
      g_sources[0] = {g_fork.fd, 0, g_mapped};
      g_num_sources = 1;
    }
    g_owner_pid = 0;  // the directory belongs to the parent
    g_writer.store(0);
    for (auto &s : g_shards) s.n.store(0);
  }
  g_fork = {kForkNone, -1, false};
  --t_busy;
}

void fork_child() { fork_child_impl(true); }

struct CloneCtx {
  int (*fn)(void *);
  void *arg;
  int flags;
};

int clone_trampoline(void *p) {
  auto *ctx = static_cast<CloneCtx *>(p);
  fork_child_impl(!(ctx->flags & CLONE_FILES));
  return ctx->fn(ctx->arg);
}

// ---------------------------------------------------------------------------
// Initialisation and teardown
// ---------------------------------------------------------------------------

std::size_t parse_size(const char *s, std::size_t dflt) {
  if (!s || !*s) return dflt;
  char *end;
  unsigned long long v = strtoull(s, &end, 10);
  switch (*end) {
    case 'T': case 't': v <<= 10; [[fallthrough]];
    case 'G': case 'g': v <<= 10; [[fallthrough]];
    case 'M': case 'm': v <<= 10; [[fallthrough]];
    case 'K': case 'k': v <<= 10; break;
    default: break;
  }
  return v ? std::size_t(v) : dflt;
}

bool env_flag(const char *name) {
  const char *v = getenv(name);
  return v && *v && strcmp(v, "0") != 0;
}

bool should_activate() {
  if (env_flag("METALL_PRELOAD_DISABLE")) return false;
  if (env_flag("METALL_PRELOAD_ALL_PROCESSES")) return true;
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) return false;
  exe[n] = '\0';
  const char *base = strrchr(exe, '/');
  base = base ? base + 1 : exe;
  return strcmp(base, "nix") == 0;
}

// Finds the descriptors Metall opened under our directory: marks them
// close-on-exec and records the segment block files as fork sources.
bool collect_metall_fds() {
  DIR *d = opendir("/proc/self/fd");
  if (!d) return false;
  const std::size_t dlen = strlen(g_dir);
  while (struct dirent *e = readdir(d)) {
    char *end;
    long fd = strtol(e->d_name, &end, 10);
    if (*end || fd < 0 || fd == dirfd(d)) continue;
    char link[64], target[4096];
    snprintf(link, sizeof(link), "/proc/self/fd/%ld", fd);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) continue;
    target[n] = '\0';
    if (strncmp(target, g_dir, dlen) != 0 || target[dlen] != '/') continue;
    fcntl(int(fd), F_SETFD, fcntl(int(fd), F_GETFD) | FD_CLOEXEC);
    const char *block = strstr(target, "/block-");
    if (block) {
      std::size_t no = strtoull(block + 7, nullptr, 10);
      if (no >= kMaxSources) continue;
      g_sources[no] = {int(fd), no * kBlockSize, kBlockSize};
      g_num_sources = std::max(g_num_sources, no + 1);
    } else if (g_num_other_fds < 8) {
      g_other_fds[g_num_other_fds++] = int(fd);
    }
  }
  closedir(d);
  for (std::size_t i = 0; i < g_num_sources; ++i)
    if (g_sources[i].len == 0) return false;  // a block was not found
  return g_num_sources * kBlockSize == g_mapped;
}

__attribute__((constructor(101))) void metall_preload_init() {
  ++t_busy;  // everything allocated here comes from the next allocator
  resolve_next();
  g_verbose = env_flag("METALL_PRELOAD_VERBOSE");
  if (!should_activate()) {
    --t_busy;
    return;
  }

  metall::logger::set_log_level(g_verbose
                                    ? metall::logger::level_filter::error
                                    : metall::logger::level_filter::silent);
  if (const char *root = getenv("METALL_PRELOAD_DIR"); root && *root)
    snprintf(g_root, sizeof(g_root), "%s", root);
  g_min_free = parse_size(getenv("METALL_PRELOAD_MIN_FREE"), kDefaultMinFree);
  std::size_t capacity =
      parse_size(getenv("METALL_PRELOAD_CAPACITY"), kDefaultCapacity);
  capacity = (capacity + kBlockSize - 1) / kBlockSize * kBlockSize;
  capacity = std::min(capacity, kMaxSources * kBlockSize);

  snprintf(g_dir, sizeof(g_dir), "%s/metall-preload-%d-XXXXXX", g_root,
           (int)getpid());
  if (!mkdtemp(g_dir)) {
    log_msg("cannot create a heap directory under %s: %s; not using Metall",
            g_root, strerror(errno));
    g_dir[0] = '\0';
    --t_busy;
    return;
  }
  g_owner_pid = getpid();

  char path[sizeof(g_dir) + 32];
  snprintf(path, sizeof(path), "%s/owner.lock", g_dir);
  int lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  // Stays open for the life of the process; collect_metall_fds() records it
  // so that fork children drop it.
  if (lock_fd >= 0) flock(lock_fd, LOCK_EX | LOCK_NB);
  reap_stale_dirs();

  bool ok = false;
  try {
    snprintf(path, sizeof(path), "%s/store", g_dir);
    g_kernel = new (g_kernel_storage) kernel_t();
    if (g_kernel->create(path, capacity)) {
      sstorage_t &storage = g_kernel->*get(KernelStorage());
      // Create and map every block now; see the file header.
      ok = storage.extend(capacity) && storage.size() == capacity;
      g_mapped = storage.size();
      g_chunk_dir = &(g_kernel->*get(KernelAlloc()).*get(AllocDir()));
    }
  } catch (...) {
    ok = false;
  }
  // A failed kernel is deliberately leaked: its destructor would sync and
  // tear down mappings we can no longer reason about.
  ok = ok && collect_metall_fds();
  if (!ok) {
    log_msg("failed to create the Metall datastore in %s; not using Metall",
            g_dir);
    remove_tree(g_dir);
    g_dir[0] = '\0';
    --t_busy;
    return;
  }

  if (env_flag("METALL_PRELOAD_UNLINK")) {
    // Files stay alive through their descriptors and mappings; the kernel
    // reclaims the space whenever and however the process exits.
    remove_tree(g_dir);
    g_dir[0] = '\0';
  }
  check_disk();

  pthread_atfork(fork_prepare, fork_parent, fork_child);
  g_seg_begin = reinterpret_cast<std::uintptr_t>(g_kernel->get_segment());
  g_seg_len = g_mapped;
  VLOG("serving the heap from %s (%zu GiB sparse segment at %p)",
       g_dir[0] ? g_dir : "(unlinked)", g_mapped >> 30,
       reinterpret_cast<void *>(g_seg_begin));
  g_active.store(true, std::memory_order_release);
  --t_busy;
}

__attribute__((destructor(101))) void metall_preload_fini() {
  // Only the directory goes: other exit-time code may still use the heap, so
  // the kernel and its mappings stay alive until the process is gone.
  if (g_dir[0] && g_owner_pid == getpid()) {
    ++t_busy;
    remove_tree(g_dir);
    g_dir[0] = '\0';
    --t_busy;
  }
  // Also sweep up after build hooks: nix SIGKILLs them once a build is
  // assigned, so they never get here themselves.
  if (g_active.load(std::memory_order_acquire)) {
    ++t_busy;
    reap_stale_dirs();
    --t_busy;
  }
}

// ---------------------------------------------------------------------------
// Boehm GC entry points (the real ones are used for foreign pointers and
// whenever Metall is not active)
// ---------------------------------------------------------------------------

template <typename T>
T real_gc(const char *name) {
  return reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}

#define REAL_GC(name, ...)            \
  using real_t = __VA_ARGS__;         \
  static real_t real = nullptr;       \
  if (!real) real = real_gc<real_t>(name)

void *new_impl(std::size_t n, std::size_t align, bool nothrow) {
  if (n == 0) n = 1;
  for (;;) {
    void *p;
    if (align <= 16) {
      p = alloc_any(n);
    } else {
      p = m_alloc(n, align);
      if (!p && next_ready()) {
        // Round up: aligned_alloc historically requires a size multiple.
        p = g_next.aligned_alloc(align, (n + align - 1) & ~(align - 1));
      }
    }
    if (p) return p;
    std::new_handler h = std::get_new_handler();
    if (!h) {
      if (nothrow) return nullptr;
      throw std::bad_alloc();
    }
    if (nothrow) {
      try {
        h();
      } catch (...) {
        return nullptr;
      }
    } else {
      h();
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// C allocation API
// ---------------------------------------------------------------------------

extern "C" {

EXPORT void *malloc(std::size_t n) noexcept { return alloc_any(n); }

EXPORT void free(void *p) noexcept { free_any(p); }

EXPORT void *calloc(std::size_t n, std::size_t m) noexcept {
  std::size_t total;
  if (__builtin_mul_overflow(n, m, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  void *p = m_alloc(total, 0);
  if (p) return std::memset(p, 0, total);
  return next_calloc(n, m);
}

EXPORT void *realloc(void *p, std::size_t n) noexcept { return realloc_any(p, n); }

EXPORT void *reallocarray(void *p, std::size_t n, std::size_t m) noexcept {
  std::size_t total;
  if (__builtin_mul_overflow(n, m, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  return realloc_any(p, total);
}

EXPORT int posix_memalign(void **out, std::size_t align, std::size_t n) noexcept {
  if (!valid_posix_align(align)) return EINVAL;
  void *p = align <= 16 ? m_alloc(n, 0) : m_alloc(n, align);
  if (!p) {
    if (!next_ready()) return ENOMEM;
    return g_next.posix_memalign(out, align, n);
  }
  *out = p;
  return 0;
}

EXPORT void *aligned_alloc(std::size_t align, std::size_t n) noexcept {
  if (align == 0 || (align & (align - 1)) != 0) {
    errno = EINVAL;
    return nullptr;
  }
  void *p = m_alloc(n, align <= 16 ? 0 : align);
  if (p) return p;
  if (!next_ready()) return nullptr;
  return g_next.aligned_alloc(align, n);
}

EXPORT void *memalign(std::size_t align, std::size_t n) noexcept {
  return memalign_any(align, n);
}

EXPORT void *valloc(std::size_t n) noexcept {
  return memalign_any(std::size_t(sysconf(_SC_PAGESIZE)), n);
}

EXPORT void *pvalloc(std::size_t n) noexcept {
  const std::size_t page = std::size_t(sysconf(_SC_PAGESIZE));
  return memalign_any(page, (n + page - 1) & ~(page - 1));
}

EXPORT std::size_t malloc_usable_size(void *p) noexcept {
  if (!p) return 0;
  if (owns(p)) return m_usable(p);
  if (is_boot(p)) return boot_size(p);
  if (!next_ready()) return 0;
  return g_next.malloc_usable_size(p);
}

// ---------------------------------------------------------------------------
// clone(): glibc's fork() runs the pthread_atfork handlers, raw clone() does
// not. nix creates sandboxed build processes with clone() without CLONE_VM.
// ---------------------------------------------------------------------------

EXPORT int clone(int (*fn)(void *), void *stack, int flags, void *arg, ...) noexcept {
  va_list ap;
  va_start(ap, arg);
  pid_t *ptid = va_arg(ap, pid_t *);
  void *tls = va_arg(ap, void *);
  pid_t *ctid = va_arg(ap, pid_t *);
  va_end(ap);
  if (!next_ready()) {
    errno = ENOSYS;
    return -1;
  }
  if (!g_active.load(std::memory_order_acquire) || (flags & CLONE_VM))
    return g_next.clone(fn, stack, flags, arg, ptid, tls, ctid);

  // A child stack inside the heap would be reverted to the snapshot (taken
  // before glibc writes fn/arg onto it) when the child remaps the heap, so
  // run the child on a private anonymous stack instead.
  void *own_stack = nullptr;
  constexpr std::size_t kOwnStackSize = 8ULL << 20;
  if (owns(stack) || owns(static_cast<char *>(stack) - 1)) {
    own_stack = mmap(nullptr, kOwnStackSize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_NORESERVE,
                     -1, 0);
    if (own_stack == MAP_FAILED) {
      errno = ENOMEM;
      return -1;
    }
    stack = static_cast<char *>(own_stack) + kOwnStackSize;
  }

  // Without CLONE_VM the child gets a copy of this stack frame, so ctx is
  // valid in the child too.
  CloneCtx ctx = {fn, arg, flags};
  fork_prepare();
  int r = g_next.clone(clone_trampoline, stack, flags, &ctx, ptid, tls, ctid);
  int saved = errno;
  // With CLONE_FILES the child shares our descriptor table and may not have
  // mapped the snapshot yet, so its descriptor must stay open.
  fork_parent_impl(!(flags & CLONE_FILES) || r < 0);
  if (own_stack) munmap(own_stack, kOwnStackSize);  // the child has a copy
  errno = saved;
  return r;
}

// ---------------------------------------------------------------------------
// Boehm GC API. The collector is kept initialised (nix calls into it for
// thread registration and statistics) but disabled; all allocations are
// served from Metall and are never collected.
// ---------------------------------------------------------------------------

typedef void (*gc_finalizer_t)(void *, void *);

EXPORT void GC_init(void) {
  REAL_GC("GC_init", void (*)(void));
  if (real) real();
  if (g_active.load(std::memory_order_acquire)) {
    auto disable = real_gc<void (*)(void)>("GC_disable");
    if (disable) disable();
  }
}

EXPORT void *GC_malloc(std::size_t n) {
  void *p = m_alloc(n, 0);
  if (p) return std::memset(p, 0, n);
  REAL_GC("GC_malloc", void *(*)(std::size_t));
  return real ? real(n) : nullptr;
}

EXPORT void *GC_malloc_atomic(std::size_t n) {
  void *p = m_alloc(n, 0);
  if (p) return p;
  REAL_GC("GC_malloc_atomic", void *(*)(std::size_t));
  return real ? real(n) : nullptr;
}

EXPORT void *GC_malloc_uncollectable(std::size_t n) {
  void *p = m_alloc(n, 0);
  if (p) return std::memset(p, 0, n);
  REAL_GC("GC_malloc_uncollectable", void *(*)(std::size_t));
  return real ? real(n) : nullptr;
}

EXPORT void *GC_malloc_atomic_uncollectable(std::size_t n) {
  void *p = m_alloc(n, 0);
  if (p) return p;
  REAL_GC("GC_malloc_atomic_uncollectable", void *(*)(std::size_t));
  return real ? real(n) : nullptr;
}

// nix <= 2.24 copies every evaluator string with GC_STRDUP.
EXPORT char *GC_strndup(const char *s, std::size_t n) {
  n = strnlen(s, n);
  if (auto *p = static_cast<char *>(m_alloc(n + 1, 0))) {
    std::memcpy(p, s, n);
    p[n] = 0;
    return p;
  }
  REAL_GC("GC_strndup", char *(*)(const char *, std::size_t));
  return real ? real(s, n) : nullptr;
}

EXPORT char *GC_strdup(const char *s) {
  if (!s) return nullptr;
  return GC_strndup(s, SIZE_MAX);
}

// Returns a list of zeroed objects linked through their first word.
EXPORT void *GC_malloc_many(std::size_t n) {
  if (g_active.load(std::memory_order_acquire) && !t_busy) {
    const std::size_t sz =
        std::max<std::size_t>(16, (n + 15) & ~std::size_t(15));
    const std::size_t count = std::max<std::size_t>(1, 4096 / sz);
    void *head = nullptr;
    for (std::size_t i = 0; i < count; ++i) {
      void *p = m_alloc(sz, 0);
      if (!p) break;
      std::memset(p, 0, sz);
      *static_cast<void **>(p) = head;
      head = p;
    }
    if (head) return head;
  }
  REAL_GC("GC_malloc_many", void *(*)(std::size_t));
  return real ? real(n) : nullptr;
}

EXPORT void GC_free(void *p) {
  if (!p) return;
  if (owns(p)) {
    m_free(p);
    return;
  }
  REAL_GC("GC_free", void (*)(void *));
  if (real) real(p);
}

EXPORT void GC_gcollect(void) {
  if (g_active.load(std::memory_order_acquire)) return;
  REAL_GC("GC_gcollect", void (*)(void));
  if (real) real();
}

EXPORT int GC_expand_hp(std::size_t n) {
  if (g_active.load(std::memory_order_acquire)) return 1;
  REAL_GC("GC_expand_hp", int (*)(std::size_t));
  return real ? real(n) : 0;
}

// Finalizers on Metall objects would never run (nothing is collected), so
// registration is a no-op that reports "no previous finalizer".
#define FINALIZER_HOOK(name)                                                 \
  EXPORT void name(void *obj, gc_finalizer_t fn, void *cd,                   \
                   gc_finalizer_t *ofn, void **ocd) {                        \
    if (owns(obj)) {                                                         \
      if (ofn) *ofn = nullptr;                                               \
      if (ocd) *ocd = nullptr;                                               \
      return;                                                                \
    }                                                                        \
    REAL_GC(#name, void (*)(void *, gc_finalizer_t, void *, gc_finalizer_t *, \
                            void **));                                       \
    if (real) real(obj, fn, cd, ofn, ocd);                                   \
  }

FINALIZER_HOOK(GC_register_finalizer)
FINALIZER_HOOK(GC_register_finalizer_ignore_self)
FINALIZER_HOOK(GC_register_finalizer_no_order)
FINALIZER_HOOK(GC_register_finalizer_unreachable)

}  // extern "C"

// ---------------------------------------------------------------------------
// C++ operator new/delete. nix's mimalloc replaces these without going
// through malloc, so they must be interposed separately.
// ---------------------------------------------------------------------------

EXPORT void *operator new(std::size_t n) { return new_impl(n, 0, false); }
EXPORT void *operator new[](std::size_t n) { return new_impl(n, 0, false); }
EXPORT void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
  return new_impl(n, 0, true);
}
EXPORT void *operator new[](std::size_t n, const std::nothrow_t &) noexcept {
  return new_impl(n, 0, true);
}
EXPORT void *operator new(std::size_t n, std::align_val_t a) {
  return new_impl(n, std::size_t(a), false);
}
EXPORT void *operator new[](std::size_t n, std::align_val_t a) {
  return new_impl(n, std::size_t(a), false);
}
EXPORT void *operator new(std::size_t n, std::align_val_t a,
                          const std::nothrow_t &) noexcept {
  return new_impl(n, std::size_t(a), true);
}
EXPORT void *operator new[](std::size_t n, std::align_val_t a,
                            const std::nothrow_t &) noexcept {
  return new_impl(n, std::size_t(a), true);
}

EXPORT void operator delete(void *p) noexcept { free_any(p); }
EXPORT void operator delete[](void *p) noexcept { free_any(p); }
EXPORT void operator delete(void *p, std::size_t) noexcept { free_any(p); }
EXPORT void operator delete[](void *p, std::size_t) noexcept { free_any(p); }
EXPORT void operator delete(void *p, std::align_val_t) noexcept {
  free_any(p);
}
EXPORT void operator delete[](void *p, std::align_val_t) noexcept {
  free_any(p);
}
EXPORT void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
  free_any(p);
}
EXPORT void operator delete[](void *p, std::size_t,
                              std::align_val_t) noexcept {
  free_any(p);
}
EXPORT void operator delete(void *p, const std::nothrow_t &) noexcept {
  free_any(p);
}
EXPORT void operator delete[](void *p, const std::nothrow_t &) noexcept {
  free_any(p);
}
EXPORT void operator delete(void *p, std::align_val_t,
                            const std::nothrow_t &) noexcept {
  free_any(p);
}
EXPORT void operator delete[](void *p, std::align_val_t,
                              const std::nothrow_t &) noexcept {
  free_any(p);
}
