#!/usr/bin/env bash
# Build the gem5-CIMD image (if needed) and drop into a shell with the source
# tree mounted, so `scons build/X86/gem5.opt` writes into the host checkout.
#
#   ./docker-run.sh                       # interactive shell in /gem5
#   ./docker-run.sh scons build/X86/gem5.opt -j$(nproc)
#
# The whole MIMDRAM repo is mounted at /workspace (workloads, microworkloads,
# m5out, ...) and gem5-CIMD itself at /gem5.
set -euo pipefail

GEM5_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$GEM5_DIR")"
IMAGE="${IMAGE:-gem5-cimd}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "==> building $IMAGE"
    docker build -t "$IMAGE" \
        --build-arg UID="$(id -u)" \
        --build-arg GID="$(id -g)" \
        "$GEM5_DIR"
fi

exec docker run -it --rm \
    -v "$REPO_DIR":/workspace \
    -v "$GEM5_DIR":/gem5 \
    -w /gem5 \
    --name "gem5-cimd-$$" \
    "$IMAGE" "$@"
