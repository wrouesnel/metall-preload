# metall-preload deployment bundle

This bundle installs metall-preload so that the system `nix-daemon` and every
user's `nix` keep their heap in a disk-backed Metall datastore instead of RAM.

```
install.sh                    installer and uninstaller (run as root)
lib/libmetall_preload.so      the library
systemd/metall-preload.conf   nix-daemon.service drop-in (template)
profile.d/metall-preload.sh   login-shell snippet (template, --shells profile)
BUILDINFO                     platform, compiler and minimum glibc
```

## Pick a bundle

The library is loaded in two places, and each uses a different glibc:

* **In nix:** against the glibc nix was built with. nix carries its own
  glibc in the nix store, and it can be older than the system's.
* **In everything else that inherits `LD_PRELOAD`:** against the system's
  glibc.

Both must be at least the glibc in the bundle's `BUILDINFO`.

| Bundle | Needs glibc | Use it for |
|---|---|---|
| `rhel8` | 2.27 | Any system and any nix. **The safe choice.** |
| `rhel10`, `ubuntu2404`, `ubuntu2604` | 2.38 | Only when your nix was built with glibc 2.38 or newer |

Some nix builds use an older glibc. Upstream nix 2.18 and earlier, as built
from nixpkgs 23.05 (and the official installer of that time), uses glibc
2.37. With a 2.38 bundle, its `nix-daemon` exits at once with "version
`GLIBC_2.38' not found". Recent Determinate Nix uses glibc 2.40 or newer.

To see which glibc your nix uses:

```sh
ldd "$(readlink -f "$(command -v nix)")" | grep libc.so
# libc.so.6 => /nix/store/...-glibc-2.37-45/lib/libc.so.6
```

`install.sh` checks both: it loads the library into `/bin/true` and into
`nix --version` (`--nix FILE` picks which nix to check), and refuses to
install if either fails. The library carries its own C++ runtime, so glibc
is all that matters.

## Install

```sh
tar xzf metall-preload-<version>-<platform>-x86_64.tar.gz
cd metall-preload-<version>-<platform>-x86_64
sudo ./install.sh
```

This:

1. Checks that both this system and its nix can load the library (see
   above), and stops if either can't.
2. Installs it as `/usr/local/lib/libmetall_preload.so`.
3. Adds `/etc/systemd/system/nix-daemon.service.d/metall-preload.conf`,
   restarts `nix-daemon.service`, and checks that the running daemon has the
   library loaded. On Determinate Nix the unit runs `determinate-nixd`, which
   passes the setting on to the `nix-daemon` it starts.
4. Adds `LD_PRELOAD` to `/etc/environment`. PAM applies it to every new
   session (ssh, including `ssh host nix ...`; console and `su` logins; cron),
   whatever the shell.

Restarting the daemon interrupts builds in progress. Use `--no-restart` to
apply the change at the next restart instead.

Then, in a new session:

```sh
$ nix --version
nix (Determinate Nix-METALL 3.24.1) 2.35.3
```

The `-METALL` tag means the heap is being served from Metall. Without it, the
library isn't loaded in that process, or it failed to start (rerun with
`METALL_PRELOAD_VERBOSE=1` to see why).

Options:

| Option | Default | |
|---|---|---|
| `--heap-dir DIR` | `/tmp` | Where the daemon's heaps go. Use a disk-backed filesystem; tmpfs puts the heap back in RAM. |
| `--shells MODE` | `pam` | `pam` (all sessions through `/etc/environment`), `profile` (login shells through `/etc/profile.d`), or `none` |
| `--no-daemon` | | Leave `nix-daemon.service` alone |
| `--no-restart` | | Don't restart the daemon now |
| `--prefix DIR` | `/usr/local/lib` | Where to install the library |
| `--lib FILE` | `lib/libmetall_preload.so` | Install a different build |
| `--nix FILE` | `/nix/var/nix/profiles/default/bin/nix` | The nix to check the library against |

The library only activates in `nix` (and its `nix-daemon`, `nix-build` and
similar entry points) and `nix-eval-jobs`. Every other program that inherits
`LD_PRELOAD` passes straight through to the normal allocator.

`sudo` resets the environment, so `sudo nix ...` runs without the library.
Root's builds still go through the daemon, which has it.

## Update

Run the new bundle's `install.sh` the same way. The library file is replaced,
not overwritten, so running processes keep the version they loaded.

## Uninstall

```sh
sudo ./install.sh --uninstall
```

This removes the session setting, the drop-in and the library, and restarts
the daemon. Sessions that were already open keep `LD_PRELOAD` until they end.
In them, `ld.so` prints a harmless "cannot be preloaded ... ignored" warning
once the library is gone.

## NixOS

NixOS builds the library from source. See `deploy/nixos/` in the source tree.
