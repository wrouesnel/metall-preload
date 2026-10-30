#!/usr/bin/env bash
# Installs metall-preload system-wide: the library, a nix-daemon.service
# drop-in, and LD_PRELOAD for every user's sessions. Run as root.
#
#   install.sh [options]              install, or update an existing install
#   install.sh --uninstall [options]  remove everything this script installs
#
# Options:
#   --lib FILE        library to install (default: lib/libmetall_preload.so
#                     next to this script, else ../build/libmetall_preload.so)
#   --prefix DIR      where to install it (default: /usr/local/lib)
#   --heap-dir DIR    where the daemon keeps its heaps (default: /tmp)
#   --shells MODE     how user sessions get LD_PRELOAD (default: pam)
#                       pam      /etc/environment: every PAM session (ssh,
#                                console, su, cron) and any shell
#                       profile  /etc/profile.d: login shells that read it
#                       none     leave user sessions alone
#   --no-daemon       leave nix-daemon.service alone
#   --no-restart      don't restart nix-daemon.service (running builds are
#                     not interrupted; the change applies at the next start)
#   -h, --help        show this help
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
prefix=/usr/local/lib
heap_dir=/tmp
shells=pam
daemon=1
restart=1
action=install
lib_src=
for candidate in "$here/lib/libmetall_preload.so" "$here/../build/libmetall_preload.so"; do
  if [ -z "$lib_src" ] && [ -f "$candidate" ]; then lib_src=$candidate; fi
done

usage() { sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; }
while [ $# -gt 0 ]; do
  case $1 in
    --lib) lib_src=$2; shift ;;
    --prefix) prefix=$2; shift ;;
    --heap-dir) heap_dir=$2; shift ;;
    --shells) shells=$2; shift ;;
    --no-daemon) daemon=0 ;;
    --no-restart) restart=0 ;;
    --uninstall) action=uninstall ;;
    -h | --help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done
case $shells in pam | profile | none) ;; *) echo "--shells must be pam, profile or none" >&2; exit 2 ;; esac
if [ "$(id -u)" != 0 ]; then
  echo "run as root" >&2
  exit 1
fi

lib=$prefix/libmetall_preload.so
dropin=/etc/systemd/system/nix-daemon.service.d/metall-preload.conf
profile=/etc/profile.d/metall-preload.sh
environment=/etc/environment
marker="# metall-preload: added by install.sh, removed by install.sh --uninstall"
nix=/nix/var/nix/profiles/default/bin/nix

say() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }

have_daemon_unit() {
  [ "$daemon" = 1 ] && command -v systemctl >/dev/null &&
    systemctl cat nix-daemon.service >/dev/null 2>&1
}

remove_environment_entry() {
  [ -f "$environment" ] || return 0
  local tmp
  tmp=$(mktemp "$environment.XXXXXX")
  grep -vxF -e "$marker" -e "LD_PRELOAD=$lib" "$environment" >"$tmp" || true
  chmod --reference="$environment" "$tmp"
  mv "$tmp" "$environment"
}

restart_daemon() {
  systemctl daemon-reload
  if [ "$restart" = 1 ]; then
    say "restarting nix-daemon.service"
    systemctl restart nix-daemon.service
  else
    say "not restarting nix-daemon.service (--no-restart)"
  fi
}

# Starts a daemon process if needed and reports whether it loaded the library.
verify_daemon() {
  [ -x "$nix" ] && "$nix" store ping --store daemon >/dev/null 2>&1 || true
  local pid
  pid=$(pgrep -o -x nix-daemon || true)
  if [ -z "$pid" ]; then
    warn "no nix-daemon process found to verify"
  elif grep -qF "$lib" "/proc/$pid/maps"; then
    say "nix-daemon (pid $pid) has the library loaded"
  else
    warn "nix-daemon (pid $pid) does not have $lib loaded"
    warn "check: systemctl show nix-daemon.service -p Environment"
  fi
}

if [ "$action" = uninstall ]; then
  say "removing LD_PRELOAD from user sessions"
  remove_environment_entry
  rm -f "$profile"
  if have_daemon_unit && [ -f "$dropin" ]; then
    say "removing $dropin"
    rm -f "$dropin"
    rmdir --ignore-fail-on-non-empty "$(dirname "$dropin")"
    restart_daemon
  fi
  say "removing $lib"
  rm -f "$lib"
  say "done. Sessions started before this still have LD_PRELOAD set until"
  say "they end; ld.so prints a harmless warning in them now the library is gone."
  exit 0
fi

if [ -z "$lib_src" ] || [ ! -f "$lib_src" ]; then
  echo "no library found; build it or pass --lib FILE" >&2
  exit 1
fi

# Before any session is pointed at it, make sure this system's dynamic linker
# can load the library (for example, that glibc is new enough).
if ! err=$(LD_PRELOAD=$lib_src /bin/true 2>&1) || [ -n "$err" ]; then
  echo "this system cannot load $lib_src:" >&2
  echo "$err" >&2
  exit 1
fi

say "installing $lib"
# install(1) writes a new file, so running processes keep the old one.
install -D -m 0755 "$lib_src" "$lib"

if have_daemon_unit; then
  say "installing $dropin (heaps in $heap_dir)"
  mkdir -p "$(dirname "$dropin")"
  sed -e "s|@LIB@|$lib|g" -e "s|@HEAP_DIR@|$heap_dir|g" \
    "$here/systemd/metall-preload.conf" >"$dropin"
  restart_daemon
  [ "$restart" = 1 ] && verify_daemon
elif [ "$daemon" = 1 ]; then
  warn "no nix-daemon.service found; skipping the daemon"
fi

case $shells in
  pam)
    if [ -f "$environment" ] && grep -q '^LD_PRELOAD=' "$environment" &&
      ! grep -qxF "LD_PRELOAD=$lib" "$environment"; then
      echo "$environment already sets a different LD_PRELOAD; not changing it" >&2
      echo "add $lib to it by hand, or use --shells profile" >&2
      exit 1
    fi
    rm -f "$profile"
    remove_environment_entry
    say "adding LD_PRELOAD to $environment"
    printf '%s\nLD_PRELOAD=%s\n' "$marker" "$lib" >>"$environment"
    ;;
  profile)
    remove_environment_entry
    say "installing $profile"
    sed -e "s|@LIB@|$lib|g" "$here/profile.d/metall-preload.sh" >"$profile"
    chmod 0644 "$profile"
    ;;
  none)
    remove_environment_entry
    rm -f "$profile"
    ;;
esac

say "done. New sessions pick it up; check with: nix --version   (shows -METALL)"
