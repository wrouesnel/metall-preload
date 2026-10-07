#!/usr/bin/env bash
# Runs the metall-preload compatibility checks against nix installations.
#
#   test/versions.sh <label>=<nix-out-path>...
#
# Each check runs natively and under the preload and compares the results.
# The daemon check starts a root nix-daemon (via sudo) that serves a throwaway
# chroot store under $WORK, so the system store is never touched.
#
# Environment: LIB (the .so), NIXPKGS (a nixpkgs source path every version can
# evaluate), WORK (scratch directory), SKIP_DAEMON=1.
set -u

LIB=${LIB:-$(realpath "$(dirname "$0")/../build/libmetall_preload.so")}
NIXPKGS=${NIXPKGS:-/nix/store/lc5bxq4vsgpjjc7i9phdm8s0bxjz0drm-source}
WORK=${WORK:-/tmp/nvtest}
# Chroot stores for the daemon check. nix >= 2.30 refuses world-writable
# parents such as /tmp, and build users must be able to reach it.
STORES=${STORES:-/var/lib/metall-nvtest}

mkdir -p "$WORK/conf"
cat >"$WORK/conf/nix.conf" <<EOF
experimental-features = nix-command flakes
substituters = https://cache.nixos.org/
sandbox = true
EOF
export NIX_CONF_DIR=$WORK/conf

PURE='let
  n = 60000;
  a = builtins.listToAttrs (builtins.genList (i: {
    name = "k${toString i}";
    value = builtins.toJSON { inherit i; s = builtins.concatStringsSep "-" (map toString (builtins.genList (x: x * i) 8)); };
  }) n);
  sorted = builtins.sort (x: y: x < y) (builtins.attrNames a);
  re = builtins.filter (x: builtins.match "k1[0-9]*7" x != null) sorted;
  fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2);
in builtins.hashString "sha256" (builtins.toJSON {
  inherit re;
  f = fib 22;
  j = map (k: (builtins.fromJSON a.${k}).s) (builtins.genList (i: "k${toString (i * 7)}") 5000);
})'

# run <log> <preload:0|1> <cmd...>: stdout to $log.out, stderr to $log.err,
# "<seconds> <maxrss KB>" to $log.time.
run() {
  local log=$1 pre=$2
  shift 2
  if [ "$pre" = 1 ]; then
    env LD_PRELOAD="$LIB" METALL_PRELOAD_VERBOSE=1 NIX_SHOW_STATS=1 \
      NIX_SHOW_STATS_PATH="$log.stats" \
      /usr/bin/time -f '%e %M' -o "$log.time" "$@" >"$log.out" 2>"$log.err"
  else
    env NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$log.stats" \
      /usr/bin/time -f '%e %M' -o "$log.time" "$@" >"$log.out" 2>"$log.err"
  fi
}

# same <name> <cmd...>: runs natively and preloaded; prints ok/FAIL detail.
same() {
  local name=$1
  shift
  run "$D/$name.native" 0 "$@"
  local rn=$?
  run "$D/$name.metall" 1 "$@"
  local rm=$?
  if [ $rn -ne 0 ]; then
    echo "native-fail"
  elif [ $rm -ne 0 ]; then
    echo "FAIL(rc=$rm)"
  elif ! cmp -s "$D/$name.native.out" "$D/$name.metall.out"; then
    echo "FAIL(differs)"
  elif ! grep -q 'serving the heap' "$D/$name.metall.err"; then
    echo "FAIL(inactive)"
  else
    echo ok
  fi
}

heap() { jq -r '.gc.heapSize // empty' "$1.stats" 2>/dev/null | numfmt --to=iec 2>/dev/null || echo -; }

daemon_check() {
  local N=$1 v=$2 root=$STORES/root-$v sock=$WORK/$v.sock
  sudo rm -rf "$root" "$sock"
  sudo mkdir -p -m 755 "$STORES" && sudo mkdir -p "$root"
  sudo env LD_PRELOAD="$LIB" METALL_PRELOAD_VERBOSE=1 NIX_CONF_DIR="$NIX_CONF_DIR" \
    NIX_DAEMON_SOCKET_PATH="$sock" \
    sh -c 'echo $$ >"$0"; exec "$1" --option store "local?root=$2"' \
    "$D/daemon.pid" "$N/bin/nix-daemon" "$root" >"$D/daemon.log" 2>&1 &
  for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
  local dpid
  dpid=$(cat "$D/daemon.pid" 2>/dev/null)
  if [ ! -S "$sock" ]; then
    echo "FAIL(no socket)"
    return
  fi
  local pids=() i
  for i in 1 2 3 4; do
    env LD_PRELOAD="$LIB" "$N/bin/nix" build --store "unix://$sock" --no-link --print-out-paths --impure --expr \
      "derivation { name = \"metall-$v-$i-$RANDOM\"; system = builtins.currentSystem;
         builder = \"/bin/sh\";
         args = [ \"-c\" \"i=0; while [ \$i -lt 20000 ]; do i=\$((i+1)); done; echo \$\$ > \$out\" ]; }" \
      >"$D/build$i.out" 2>"$D/build$i.err" &
    pids+=($!)
  done
  local ok=0
  for i in 1 2 3 4; do
    wait "${pids[$((i - 1))]}" && [ "$(sudo cat "$root$(cat "$D/build$i.out")" 2>/dev/null)" = 1 ] &&
      ok=$((ok + 1))
  done
  sudo kill -TERM "$dpid" 2>/dev/null
  for _ in $(seq 100); do sudo kill -0 "$dpid" 2>/dev/null || break; sleep 0.1; done
  local act=no left
  grep -q 'serving the heap' "$D/daemon.log" && act=yes
  left=$(ls -d /tmp/metall-preload-* 2>/dev/null | wc -l)
  if [ "$act" = yes ] && [ $ok = 4 ] && [ "$left" = 0 ]; then
    echo ok
  else
    echo "FAIL(active=$act builds=$ok/4 leftover=$left)"
  fi
  sudo rm -rf "$root" "$sock"
}

printf '%-10s %-8s %-8s %-8s %-8s %-8s %-8s %-20s %-17s %-15s %s\n' \
  version active pure ffox inst flake par 'ffox time n/m' 'ffox rss n/m' 'boehm n/m' daemon
for arg in "$@"; do
  v=${arg%%=*}
  N=$(readlink -f "${arg#*=}")
  D=$WORK/logs/$v
  rm -rf "$D"
  mkdir -p "$D"

  act=no
  env LD_PRELOAD="$LIB" METALL_PRELOAD_VERBOSE=1 "$N/bin/nix" --version 2>&1 |
    grep -q 'serving the heap' && act=yes
  pure=$(same pure "$N/bin/nix-instantiate" --eval --strict -E "$PURE")
  ffox=$(same ffox "$N/bin/nix-instantiate" --eval -E "(import $NIXPKGS {}).firefox.drvPath")
  inst=$(same inst "$N/bin/nix-instantiate" -E "(import $NIXPKGS {}).hello")
  flake=$(same flake "$N/bin/nix" eval --raw "path:$NIXPKGS#legacyPackages.x86_64-linux.hello.drvPath")
  # Determinate searches in parallel with eval-cores; others ignore it.
  par=$(same par "$N/bin/nix" search --no-eval-cache --json --option eval-cores 8 \
    "path:$NIXPKGS#legacyPackages.x86_64-linux.python3Packages" '^a')
  read -r tn rn < <(tail -1 "$D/ffox.native.time")
  read -r tm rm < <(tail -1 "$D/ffox.metall.time")
  if [ -n "${SKIP_DAEMON:-}" ]; then dmn=skipped; else dmn=$(daemon_check "$N" "$v"); fi
  printf '%-10s %-8s %-8s %-8s %-8s %-8s %-8s %-20s %-17s %-15s %s\n' "$v" "$act" "$pure" "$ffox" \
    "$inst" "$flake" "$par" "${tn}s/${tm}s" "$((rn / 1024))M/$((rm / 1024))M" \
    "$(heap "$D/ffox.native")/$(heap "$D/ffox.metall")" "$dmn"
done
