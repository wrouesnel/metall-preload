#!/usr/bin/env bash
# Builds a metall-preload release bundle. Runs as root inside a container for
# the target platform: see .github/workflows/release.yml, or
# packaging/build-all.sh to run every platform locally with podman.
#
#   packaging/build-release.sh <platform> <version> [outdir]
#
# platform: rhel8 | rhel10 | ubuntu2404 | ubuntu2604
#
# Produces <outdir>/metall-preload-<version>-<platform>-x86_64.tar.gz and a
# .sha256 file next to it.
set -euo pipefail

if [ $# -lt 2 ]; then
  sed -n '2,12p' "$0" >&2
  exit 2
fi
platform=$1
version=$2
src=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "${3:-$src/dist}" && cd "${3:-$src/dist}" && pwd)
work=$(mktemp -d /tmp/metall-release.XXXXXX)
trap 'rm -rf "$work"' EXIT

# Boost headers for platforms whose packaged Boost is older than 1.80.
BOOST_URL=https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2
BOOST_SHA256=1bed88e40401b2cb7a1f76d4bab499e352fa4d0c5f31c0dbae64e24d34d7513b

cmake_args=(-DCMAKE_BUILD_TYPE=Release)

fetch_boost() {
  curl -fsSL -o "$work/boost.tar.bz2" "$BOOST_URL"
  echo "$BOOST_SHA256  $work/boost.tar.bz2" | sha256sum -c --quiet
  # Metall only needs the headers.
  tar -xjf "$work/boost.tar.bz2" -C "$work" boost_1_86_0/boost
  cmake_args+=(-DBOOST_ROOT="$work/boost_1_86_0" -DBoost_NO_SYSTEM_PATHS=ON)
}

echo "==> installing build dependencies for $platform"
case $platform in
  rhel8)
    dnf -y -q install dnf-plugins-core
    dnf config-manager --set-enabled powertools
    dnf -y -q install gcc-toolset-13-gcc-c++ gcc-toolset-13-libstdc++-devel \
      cmake make curl bzip2 binutils
    # The toolset's enable script is not written for set -u.
    set +u
    . /opt/rh/gcc-toolset-13/enable
    set -u
    fetch_boost
    ;;
  rhel10)
    dnf -y -q install dnf-plugins-core
    dnf config-manager --set-enabled crb
    dnf -y -q install gcc-c++ libstdc++-static cmake make boost-devel binutils
    ;;
  ubuntu2404 | ubuntu2604)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq g++ cmake make libboost-dev binutils >/dev/null
    ;;
  *)
    echo "unknown platform: $platform" >&2
    exit 2
    ;;
esac

echo "==> building"
cmake -S "$src" -B "$work/build" "${cmake_args[@]}"
cmake --build "$work/build" -j"$(nproc)"
lib=$work/build/libmetall_preload.so

echo "==> checking the library"
# Only glibc: the C++ runtime is linked in (METALL_PRELOAD_STATIC_RUNTIME).
foreign=$(objdump -p "$lib" | awk '/NEEDED/ {print $2}' |
  grep -vxE 'libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|ld-linux-x86-64\.so\.2' || true)
if [ -n "$foreign" ]; then
  echo "unexpected dependencies outside glibc: $foreign" >&2
  exit 1
fi
leaked=$(nm -DC --defined-only "$lib" | cut -d' ' -f3- |
  grep -vxE '(malloc|free|calloc|realloc|reallocarray|posix_memalign|aligned_alloc|memalign|valloc|pvalloc|malloc_usable_size|clone|fork|fwrite|GC_[a-z_]+|operator (new|delete).*)' || true)
if [ -n "$leaked" ]; then
  echo "unexpected exported symbols:" >&2
  echo "$leaked" >&2
  exit 1
fi
glibc=$(objdump -T "$lib" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)

echo "==> stress test"
g++ -O1 -std=c++20 -pthread "$src/test/stress.cpp" -o "$work/stress"
METALL_PRELOAD_ALL_PROCESSES=1 LD_PRELOAD="$lib" "$work/stress"

echo "==> packaging"
name=metall-preload-$version-$platform-x86_64
stage=$work/$name
mkdir -p "$stage/lib"
install -m 0755 "$lib" "$stage/lib/libmetall_preload.so"
cp -r "$src/deploy/." "$stage/"
rm -rf "$stage/nixos"  # built from source; see the source tree
cat >"$stage/BUILDINFO" <<EOF
version:   $version
platform:  $platform
built on:  $(. /etc/os-release && echo "$PRETTY_NAME")
compiler:  $(c++ --version | head -1)
requires:  $glibc or newer
commit:    ${GITHUB_SHA:-$(git -C "$src" rev-parse HEAD 2>/dev/null || echo unknown)}
EOF
tar -C "$work" --owner=0 --group=0 -czf "$out/$name.tar.gz" "$name"
(cd "$out" && sha256sum "$name.tar.gz" >"$name.tar.gz.sha256")
echo "==> $out/$name.tar.gz"
cat "$stage/BUILDINFO"
