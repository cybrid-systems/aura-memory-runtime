#!/usr/bin/env bash
# Build + test inside the same image Aura CI uses (GCC 16, C++26 contracts).
set -euo pipefail
IMAGE=${IMAGE:-ghcr.io/cybrid-systems/dev:v1.0.9}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DOCKER=${DOCKER:-docker}
exec $DOCKER run --rm -v "$ROOT":/src -w /src --entrypoint bash "$IMAGE" -c '
  set -e
  git config --global --add safe.directory "*" || true
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build build -j"$(nproc)"
  ctest --test-dir build --output-on-failure -j"$(nproc)"
  ./build/amr_bench "${BENCH_SCALE:-1}"
'
