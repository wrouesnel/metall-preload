# Nix Metall LD_PRELOAD

This is an LD_PRELOAD library which uses the [LLNL metall](https://github.com/llnl/metall) persistent memory allocator to replace usage of malloc and Boehm GC in Nix.

This allows for _enormous_ memory efficient allocations on RAM constrained hardware,
which provides massive reductions in the memory usage of nix evaluation.

## Building

Requires CMake >= 3.20, a C++20 compiler and Boost >= 1.80 headers. Metall is
fetched automatically.

```sh
cmake -S . -B build
cmake --build build
# -> build/libmetall_preload.so
```

## Usage

```sh
L=$PWD/build/libmetall_preload.so

# a single evaluation
LD_PRELOAD=$L nix eval nixpkgs#firefox.drvPath

# a manually started daemon (does not touch the systemd service)
sudo env LD_PRELOAD=$L NIX_DAEMON_SOCKET_PATH=/tmp/test-daemon.sock nix daemon
NIX_REMOTE=unix:///tmp/test-daemon.sock nix build ...
```

To use it for the system daemon, add a drop-in to `nix-daemon.service` with
`Environment=LD_PRELOAD=/path/to/libmetall_preload.so`. Make sure the library
is readable by root and that it lives somewhere stable.

The library only activates in processes whose `/proc/self/exe` is named `nix`.
Every other program that inherits `LD_PRELOAD` (build hooks, `git`, `ssh`,
builders and so on) passes straight through to the normal allocator.

### Environment

| Variable | Default | Meaning |
|---|---|---|
| `METALL_PRELOAD_DISABLE` | unset | Pass everything through to the normal allocator |
| `METALL_PRELOAD_ALL_PROCESSES` | unset | Activate in any process, not just `nix` (used by the tests) |
| `METALL_PRELOAD_DIR` | `/tmp` | Where heap directories are created |
| `METALL_PRELOAD_CAPACITY` | `256G` | Size of the sparse heap file (`K`/`M`/`G`/`T` suffixes) |
| `METALL_PRELOAD_MIN_FREE` | `1G` | When the filesystem has less free space than this, new allocations fall back to malloc |
| `METALL_PRELOAD_UNLINK` | unset | Unlink the heap files once they are mapped |
| `METALL_PRELOAD_VERBOSE` | unset | Log setup, fork snapshots and fallbacks to stderr |

## How it works

* **Interposed functions.** `malloc` and the rest of its family, every
  `operator new`/`delete` variant, and the Boehm `GC_*` API are replaced so
  that they allocate from one Metall manager. The manager lives in
  `/tmp/metall-preload-<pid>-XXXXXX/`. nix links mimalloc statically, which
  is why the C++ operators are interposed as well.
* **Boehm GC.** Boehm GC is initialised and then disabled. `GC_malloc*`
  returns zeroed Metall memory. `GC_gcollect` does nothing. Finalizers on
  Metall objects are never run.
* **Pre-extended heap.** The whole capacity is reserved up front as one
  sparse file mapping, so pointer ownership is a range check. Metall never
  needs to grow the mapping.
* **fork() and clone() without CLONE_VM.** This covers builds, sandboxes and
  crash reporters.
  * On fork, the heap file is cloned into an anonymous `O_TMPFILE`. This
    uses a reflink if the filesystem supports it, or a sparse copy otherwise.
  * The child maps that copy over its heap, so parent and child cannot see
    each other's writes.
  * `clone()` calls whose stack is inside the heap are given a private stack
    instead.
* **Cleanup.**
  * The heap directory is removed when the owning process exits.
  * A process that is SIGKILLed leaves its directory behind. The next process
    to start removes it if the owning PID is dead, or if the directory is
    unlocked and older than 60s.
* **Fallback.**
  * These requests go to the normal allocator:
    * alignments above 4096
    * requests made while the heap is nearly full
    * requests made while the disk is low on space
  * `free` sends each pointer back to the allocator that owns it.

## Caveats

* **No garbage collection.** Memory the evaluator would normally collect
  stays in the heap until the process exits. For a long-lived daemon, the
  heap grows with the evaluations done inside it. Size `METALL_PRELOAD_CAPACITY`
  and the filesystem that holds `METALL_PRELOAD_DIR` to match.
* **Where pages live.** Pages live in the page cache and are backed by the
  heap file, not by swap. On a tmpfs `/tmp` they still count as RAM or swap.
  Use a disk-backed `METALL_PRELOAD_DIR` to take the load off RAM.
* **Speed.** Small allocations are about 5x slower than mimalloc. Each fork
  costs a snapshot, measured at 0–4 ms here.

## Measurements

These were measured with Determinate nix 3.22.5.

| Workload | native | metall-preload |
|---|---|---|
| `nix eval nixpkgs#firefox.drvPath` | 3.0 s | 4.5 s (identical result) |
| outPath of every attribute of nixpkgs, `eval-cores = 4` | 247 s, 21.3 GB RSS | 193 s, 14.5 GB RSS (identical result) |
| `test/stress.cpp` | 7.6 s | ~48 s |

## Tests

```sh
g++ -O1 -g -std=c++20 -pthread test/stress.cpp -o build/stress
METALL_PRELOAD_ALL_PROCESSES=1 LD_PRELOAD=$PWD/build/libmetall_preload.so build/stress
```
