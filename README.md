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
  * The copy may not fit on disk (the free space would drop below
    `METALL_PRELOAD_MIN_FREE`), or it may fail. If the heap's data then fits
    in available memory, the parent waits while the child copies the heap
    into private memory. Otherwise the child exits at once and `fork()` or
    `clone()` fails with `ENOMEM`.
  * A child never keeps a view of the parent's heap files. The parent goes on
    writing to them, and the child would see those writes.
* **Cleanup.**
  * The heap directory is removed when the owning process exits.
  * A process that is SIGKILLed leaves its directory behind. nix does this to
    every build hook. Any preloaded process removes such directories when it
    starts or exits, if the owning PID is dead or if the directory is
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
  costs a snapshot, measured at 0–4 ms here. Parallel evaluation
  (`eval-cores`) works but scales poorly. A `nix search` with 8 cores took
  2.4 s wall and 10.3 s CPU under the preload, against 1.2 s and 2.6 s
  natively.
* **Dynamic builds only.** `LD_PRELOAD` has no effect on statically linked
  nix (for example `nix-cli-static`).

## Compatibility

`test/versions.sh` was run against these builds:

* upstream nix 2.18.9, 2.19.7, 2.20.9, 2.21.5, 2.22.4, 2.23.4, 2.24.15,
  2.25.5, 2.26.4, 2.28.7, 2.29.4, 2.30.5, 2.31.5, 2.32.8, 2.33.6, 2.34.8 and
  2.35.2, all from nixpkgs (there is no 2.27 in nixpkgs)
* Determinate Nix 3.0.0, 3.4.2, 3.8.6, 3.12.2, 3.16.3, 3.20.0, 3.22.5 and
  3.23.1

Every check passed on every version:

* The library activates.
* A pure evaluation stress test (strings, attrsets, JSON, regexes, sorting,
  recursion) gives the same result as without the preload.
* nixpkgs `firefox.drvPath` gives the same result. `nix-instantiate` of
  `hello` through the system daemon gives the same result. A `path:` flake
  eval gives the same result. So does a `nix search` with `eval-cores = 8`.
* A preloaded root `nix-daemon` serves a throwaway chroot store, and a
  preloaded client runs 4 concurrent sandboxed builds through it. Every
  build sees `$$ = 1`. No heap directories remain after the daemon exits.

A NixOS toplevel evaluation (nginx, postgresql and docker enabled) gave the
same `drvPath` as without the preload on every version. It took 3.5–4.3 s
under the preload, against 2.4–4.5 s natively.

These are the `GC_*` entry points nix uses across these versions that the
library does not interpose. All of them are thread or stack registration,
statistics, or configuration, and are safe to pass to the real libgc with
collection disabled:

* `GC_add_roots`, `GC_remove_roots`, `GC_allow_register_threads`,
  `GC_register_my_thread`, `GC_unregister_my_thread`, `GC_register_stack`,
  `GC_unregister_stack`, `GC_get_stack_base`, `GC_set_sp_corrector`
* `GC_get_heap_usage_safe`, `GC_get_bytes_since_gc`, `GC_get_gc_no`,
  `GC_get_full_gc_total_time`, `GC_start_performance_measurement`
* `GC_set_oom_fn`, `GC_set_warn_proc`, `GC_set_all_interior_pointers`,
  `GC_set_no_dls`, `GC_register_displacement`
* `GC_enable`/`GC_disable`. nix 2.18–2.23 calls these in balanced pairs, so
  the library's extra `GC_disable` keeps collection off.
* `GC_base`. This returns NULL for Metall pointers, so `gc_cleanup` skips
  registering a finalizer.

nix up to 2.24 copies every evaluator string with `GC_strdup`, which the
library interposes.

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

# fork fallback: the heap on a filesystem too small for a snapshot
g++ -O1 -g -std=c++20 test/fork_fallback.cpp -o build/fork_fallback
sudo mount -t tmpfs -o size=300m,mode=1777 tmpfs /mnt/small
METALL_PRELOAD_ALL_PROCESSES=1 METALL_PRELOAD_DIR=/mnt/small METALL_PRELOAD_MIN_FREE=1M \
  LD_PRELOAD=$PWD/build/libmetall_preload.so build/fork_fallback

# version matrix; needs sudo for the daemon check (SKIP_DAEMON=1 to skip)
nix build --no-link --print-out-paths github:NixOS/nixpkgs/nixos-24.11#nixVersions.nix_2_18
test/versions.sh 2.18=/nix/store/...-nix-2.18.9 3.23.1=/nix/store/...-determinate-nix-3.23.1
```

The version matrix evaluates a nixpkgs source tree that every version can
read (`NIXPKGS`, default nixos-24.11). Upstream builds come from the nixpkgs
release branches:

* nixos-24.11: 2.18–2.23, 2.25 and 2.26
* nixos-25.05: 2.24
* nixos-25.11: 2.28–2.30, 2.32 and 2.33
* nixos-26.05: 2.31, 2.34 and 2.35 Determinate builds are
`github:DeterminateSystems/nix-src/v<version>#nix-cli`. Only the latest
release is in its binary cache, so older ones are built from source.
