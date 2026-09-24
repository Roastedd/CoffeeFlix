#!/usr/bin/env bash
# Build CoffeeFlix in the official devkitPro container, no local toolchain needed.
#   tools/docker-build.sh            # build deps (first run only) + app
#   tools/docker-build.sh clean      # any extra args are passed to make
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${DEVKITPPC_IMAGE:-devkitpro/devkitppc:latest}"

docker run --rm -e JOBS -v "$ROOT":/src -w /src "$IMAGE" \
    bash -c 'git config --global --add safe.directory "*" && ./tools/build-deps.sh && make -j"$(nproc)" "$@"' _ "$@"
