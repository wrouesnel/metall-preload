// Checks fork isolation when the heap cannot be snapshotted. Run with the heap
// on a filesystem too small for a snapshot, e.g.
//   sudo mount -t tmpfs -o size=300m,mode=1777 tmpfs /mnt/small
//   METALL_PRELOAD_ALL_PROCESSES=1 METALL_PRELOAD_DIR=/mnt/small \
//     METALL_PRELOAD_MIN_FREE=1M LD_PRELOAD=.../libmetall_preload.so \
//     ./fork_fallback
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      abort();                                                           \
    }                                                                    \
  } while (0)

static char *g_obj;

static int files_child(void *) { return 0; }

int main() {
  // More heap than the filesystem has room left for, so no snapshot fits.
  constexpr size_t kBallast = 200 << 20;
  auto *ballast = static_cast<unsigned char *>(malloc(kBallast));
  for (size_t i = 0; i < kBallast; ++i) ballast[i] = static_cast<unsigned char>(i * 7);
  g_obj = static_cast<char *>(malloc(64));
  strcpy(g_obj, "before fork");

  // fork(): the child copies the heap into memory and must not see the
  // parent's later writes.
  int go[2];
  CHECK(pipe(go) == 0);
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    char c;
    CHECK(read(go[0], &c, 1) == 1);  // the parent has written by now
    CHECK(strcmp(g_obj, "before fork") == 0);
    for (size_t i = 0; i < kBallast; i += 4093)
      CHECK(ballast[i] == static_cast<unsigned char>(i * 7));
    // The private heap still allocates, and forks like ordinary memory.
    char *again = static_cast<char *>(malloc(1 << 20));
    CHECK(again);
    memset(again, 3, 1 << 20);
    pid_t grandchild = fork();
    CHECK(grandchild >= 0);
    if (grandchild == 0) {
      strcpy(g_obj, "grandchild");
      _exit(again[12345] == 3 ? 0 : 1);
    }
    int st;
    CHECK(waitpid(grandchild, &st, 0) == grandchild);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(strcmp(g_obj, "before fork") == 0);
    free(again);
    _exit(0);
  }
  strcpy(g_obj, "parent wrote this after fork");
  memset(ballast, 0xff, kBallast);
  CHECK(write(go[1], "x", 1) == 1);
  int st;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  puts("fork: child isolated");

  // clone(CLONE_FILES) cannot be waited for, so without a snapshot it must
  // fail cleanly rather than share the heap.
  static char stack[64 << 10];
  pid = clone(files_child, stack + sizeof(stack), CLONE_FILES | SIGCHLD, nullptr);
  if (pid < 0) {
    CHECK(errno == ENOMEM);
    puts("clone(CLONE_FILES): ENOMEM");
  } else {
    CHECK(waitpid(pid, &st, 0) == pid);
    puts("clone(CLONE_FILES): ran (snapshot available)");
  }
  puts("fork_fallback: OK");
  return 0;
}
