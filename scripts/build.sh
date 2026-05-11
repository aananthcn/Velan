#!/usr/bin/env bash

# Copyright 2026 Aananth C N
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


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
