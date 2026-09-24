// Exercises metall-preload outside nix. Run with
//   METALL_PRELOAD_ALL_PROCESSES=1 LD_PRELOAD=.../libmetall_preload.so ./stress
#include <malloc.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      abort();                                                           \
    }                                                                    \
  } while (0)

static void worker(int seed) {
  std::mt19937 rng(seed);
  std::vector<std::pair<unsigned char *, size_t>> live;
  for (int i = 0; i < 200000; ++i) {
    const int op = rng() % 10;
    if (op < 4 || live.empty()) {
      size_t n = rng() % 10 == 0 ? rng() % (1 << 20) : rng() % 512;
      auto *p = static_cast<unsigned char *>(malloc(n));
      CHECK(p || n == 0);
      CHECK(reinterpret_cast<uintptr_t>(p) % 16 == 0);
      memset(p, seed & 0xff, n);
      live.emplace_back(p, n);
    } else if (op < 8) {
      size_t k = rng() % live.size();
      auto [p, n] = live[k];
      for (size_t j = 0; j < n; j += 97) CHECK(p[j] == (seed & 0xff));
      free(p);
      live[k] = live.back();
      live.pop_back();
    } else {
      size_t k = rng() % live.size();
      auto &[p, n] = live[k];
      size_t nn = rng() % 4096;
      auto *q = static_cast<unsigned char *>(realloc(p, nn ? nn : 1));
      CHECK(q);
      for (size_t j = 0; j < std::min(n, nn); j += 31)
        CHECK(q[j] == (seed & 0xff));
      memset(q, seed & 0xff, nn);
      p = q;
      n = nn;
    }
  }
  for (auto &[p, n] : live) free(p);
}

static int clone_child(void *arg) {
  auto *shared = static_cast<int *>(arg);
  CHECK(*shared == 42);
  *shared = 7;  // must not be visible to the parent
  auto *s = new std::string(1000, 'c');
  delete s;
  return 0;
}

static double now() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
static double g_t0 = now();
#define PHASE(name) fprintf(stderr, "[%7.2fs] %s\n", now() - g_t0, name)

int main() {
  // Basic API coverage.
  void *p = calloc(1000, 8);
  for (int i = 0; i < 8000; ++i) CHECK(static_cast<char *>(p)[i] == 0);
  CHECK(malloc_usable_size(p) >= 8000);
  free(p);
  volatile size_t huge = SIZE_MAX / 2;
  CHECK(calloc(huge, 4) == nullptr);
  for (size_t a : {16, 32, 64, 256, 4096, 8192, 65536}) {
    void *q = nullptr;
    CHECK(posix_memalign(&q, a, 100) == 0);
    CHECK(reinterpret_cast<uintptr_t>(q) % a == 0);
    free(q);
    q = aligned_alloc(a, a * 3);
    CHECK(reinterpret_cast<uintptr_t>(q) % a == 0);
    free(q);
    q = memalign(a, 10);
    CHECK(reinterpret_cast<uintptr_t>(q) % a == 0);
    free(q);
  }
  struct alignas(128) Big {
    char c[300];
  };
  auto *b = new Big;
  CHECK(reinterpret_cast<uintptr_t>(b) % 128 == 0);
  delete b;
  void *big = malloc(300 << 20);
  CHECK(big);
  memset(big, 1, 300 << 20);
  free(big);
  CHECK(realloc(malloc(10), 0) == nullptr);

  PHASE("threads");
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) threads.emplace_back(worker, t + 1);
  for (auto &t : threads) t.join();

  PHASE("fork");
  // fork(): the child must get its own copy of the heap.
  int *shared = static_cast<int *>(malloc(sizeof(int)));
  *shared = 42;
  std::atomic<bool> stop{false};
  std::thread background([&] {
    while (!stop) free(malloc(64));
  });
  for (int i = 0; i < 20; ++i) {
    pid_t pid = fork();
    if (pid == 0) {
      CHECK(*shared == 42);
      *shared = 1;
      worker(99);
      int *again = static_cast<int *>(malloc(sizeof(int)));
      *again = 5;
      free(again);
      _exit(0);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(*shared == 42);
  }
  stop = true;
  background.join();

  PHASE("clone");
  // Raw clone() without CLONE_VM, the way nix starts sandboxed builds. The
  // stack deliberately lives in the (Metall) heap.
  std::vector<char> stack(1 << 20);
  pid_t pid = clone(clone_child, stack.data() + stack.size(), SIGCHLD, shared);
  CHECK(pid > 0);
  int status;
  CHECK(waitpid(pid, &status, 0) == pid);
  if (!WIFEXITED(status)) fprintf(stderr, "clone child signal %d\n", WTERMSIG(status));
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(*shared == 42);
  free(shared);

  puts("stress: OK");
  return 0;
}
