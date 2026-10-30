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

Use the bundle built for your distribution. Any bundle works on a system
whose glibc is at least the one in its `BUILDINFO`. The `rhel8` bundle needs
only glibc 2.27, so it is the fallback for anything else. The library carries
its own C++ runtime, so it works with every nix version from 2.18 on,
including Determinate Nix.

## Install

```sh
tar xzf metall-preload-<version>-<platform>-x86_64.tar.gz
cd metall-preload-<version>-<platform>-x86_64
sudo ./install.sh
```

This:

1. Checks that this system can load the library, and stops if it can't.
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
