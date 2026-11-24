#!/usr/bin/env bash
# Build + test inside the same image Aura CI uses (GCC 16, C++26 contracts).
set -euo pipefail
IMAGE=${IMAGE:-ghcr.io/cybrid-systems/aura-ci@sha256:66392ce2a5f24668fb923d1c900c545435f6b453f3df585dc4668402c07042d5}
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
