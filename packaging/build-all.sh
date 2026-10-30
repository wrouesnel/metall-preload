#!/usr/bin/env bash
# Builds every platform's release bundle locally, in the same containers the
# release workflow uses.
#
#   packaging/build-all.sh [version] [outdir]
#
# Needs podman (or set CONTAINER=docker). Logs go to <outdir>/<platform>.log.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
version=${1:-0.0.0-$(git -C "$src" rev-parse --short=12 HEAD 2>/dev/null || echo local)}
out=$(mkdir -p "${2:-$src/dist}" && cd "${2:-$src/dist}" && pwd)
engine=${CONTAINER:-podman}

# Keep in step with .github/workflows/release.yml.
platforms=(
  rhel8=docker.io/library/almalinux:8
  rhel10=docker.io/library/almalinux:10
  ubuntu2404=docker.io/library/ubuntu:24.04
  ubuntu2604=docker.io/library/ubuntu:26.04
)

pids=()
for entry in "${platforms[@]}"; do
  platform=${entry%%=*}
  image=${entry#*=}
  echo "==> $platform ($image)"
  "$engine" run --rm -v "$src:/src:ro" -v "$out:/out" -e GITHUB_SHA="$(git -C "$src" rev-parse HEAD 2>/dev/null || true)" \
    "$image" /src/packaging/build-release.sh "$platform" "$version" /out \
    >"$out/$platform.log" 2>&1 &
  pids+=("$platform:$!")
done

failed=0
for entry in "${pids[@]}"; do
  if wait "${entry#*:}"; then
    echo "ok      ${entry%%:*}"
  else
    echo "FAILED  ${entry%%:*} (see $out/${entry%%:*}.log)"
    failed=1
  fi
done
ls -l "$out"/*.tar.gz
exit $failed
