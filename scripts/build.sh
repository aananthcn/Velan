#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

CUDA=OFF
JOBS=$(nproc)

usage() {
    echo "Usage: $(basename "$0") [--cuda] [-j <jobs>]"
    echo "  --cuda      Enable CUDA acceleration (default: off)"
    echo "  -j <jobs>   Parallel jobs (default: $(nproc))"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cuda) CUDA=ON ;;
        -j)     JOBS="$2"; shift ;;
        --help) usage; exit 0 ;;
        *)      echo "Unknown option: $1"; usage; exit 1 ;;
    esac
    shift
done

echo "[build] Configuring (CUDA=${CUDA})..."
cmake -B "$ROOT_DIR/build" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DGGML_CUDA="$CUDA" \
    -S "$ROOT_DIR"

echo "[build] Building with $JOBS jobs..."
cmake --build "$ROOT_DIR/build" -j"$JOBS"

echo "[build] Done. Binary: build/velan"
