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

TARGET=""
AICORE=""     # set after parsing; defaults to cuda (pc) or cpu (rpi)
JOBS=$(nproc)

usage() {
    echo "Usage: $(basename "$0") --target <pc|rpi> [--aicore <cpu|cuda|hailo8>] [-j <jobs>]"
    echo "  --target <pc|rpi>              Build target: pc (local dev) or rpi (Raspberry Pi)  [required]"
    echo "  --aicore <cpu|cuda|hailo8>     AI accelerator (default: cuda for pc, hailo8 for rpi)"
    echo "  -j <jobs>                      Parallel jobs (default: $(nproc))"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)
            TARGET="$2"
            if [[ "$TARGET" != "pc" && "$TARGET" != "rpi" ]]; then
                echo "Error: --target must be 'pc' or 'rpi', got '$TARGET'"
                usage; exit 1
            fi
            shift ;;
        --aicore)
            AICORE="$2"
            if [[ "$AICORE" != "cpu" && "$AICORE" != "cuda" && "$AICORE" != "hailo8" ]]; then
                echo "Error: --aicore must be 'cpu', 'cuda', or 'hailo8', got '$AICORE'"
                usage; exit 1
            fi
            shift ;;
        -j)     JOBS="$2"; shift ;;
        --help) usage; exit 0 ;;
        *)      echo "Unknown option: $1"; usage; exit 1 ;;
    esac
    shift
done

if [[ -z "$TARGET" ]]; then
    echo "Error: --target is required"
    usage; exit 1
fi

# Apply target-specific default for --aicore when not explicitly set.
if [[ -z "$AICORE" ]]; then
    case "$TARGET" in
        pc)  AICORE="cuda"   ;;
        rpi) AICORE="hailo8" ;;
    esac
fi

# ---------------------------------------------------------------------------
# Conan is required
# ---------------------------------------------------------------------------
command -v conan >/dev/null 2>&1 || {
    echo "[build] ERROR: conan not found."
    echo "[build]        Install with: pip install conan"
    exit 1
}

GGML_CUDA=OFF
GGML_HAILO8=OFF
CONAN_OPTS=()

case "$AICORE" in
    cuda)   GGML_CUDA=ON;   CONAN_OPTS+=("-o" "cuda=True") ;;
    hailo8) GGML_HAILO8=ON; CONAN_OPTS+=("-o" "hailo8=True") ;;
esac

BUILD_DIR="$ROOT_DIR/build/$TARGET"

# ---------------------------------------------------------------------------
# Target-specific profile selection
# ---------------------------------------------------------------------------
case "$TARGET" in
    pc)
        PROF_HOST="$ROOT_DIR/profiles/pc"
        PROF_BUILD="$ROOT_DIR/profiles/pc"
        ;;
    rpi)
        command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || {
            echo "[build] ERROR: aarch64-linux-gnu-gcc not found."
            echo "[build]        Install with: sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu"
            exit 1
        }
        PROF_HOST="$ROOT_DIR/profiles/rpi"
        PROF_BUILD="$ROOT_DIR/profiles/pc"
        ;;
esac

echo "[build] Target=${TARGET}, AI core=${AICORE}, CUDA=${GGML_CUDA}, Hailo8=${GGML_HAILO8}"
echo "[build] Build directory: build/${TARGET}"

# ---------------------------------------------------------------------------
# Wipe stale CMake cache when switching between pc and rpi targets.
# Must happen BEFORE conan install so we don't delete the freshly-generated
# conan_toolchain.cmake. Conan's toolchain sets CMAKE_CXX_COMPILER; the
# aarch64 string check works for both old and Conan-generated toolchains.
# ---------------------------------------------------------------------------
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_cxx=$(grep -s "^CMAKE_CXX_COMPILER:" "$BUILD_DIR/CMakeCache.txt" \
                 | cut -d= -f2 || true)
    case "$TARGET" in
        pc)
            if [[ "$cached_cxx" == *"aarch64"* ]]; then
                echo "[build] Stale cache (was rpi) — wiping build/${TARGET}..."
                rm -rf "$BUILD_DIR"
            fi
            ;;
        rpi)
            if [[ -z "$cached_cxx" || "$cached_cxx" != *"aarch64"* ]]; then
                echo "[build] Stale cache (was pc) — wiping build/${TARGET}..."
                rm -rf "$BUILD_DIR"
            fi
            ;;
    esac
fi

# ---------------------------------------------------------------------------
# Local Conan recipes — create once, Conan caches the result.
# Re-runs are fast (no-op if package already in ~/.conan2 cache).
# ---------------------------------------------------------------------------

# portaudio: upstream recipe auto-detects JACK from the build host and compiles
# pa_jack.c, but never declares libjack in system_libs — causing linker failures
# when cross-compiling for aarch64.  Our local recipe adds PA_USE_JACK=OFF.
echo "[build] Conan: creating portaudio package (cached after first run)..."
conan create "$ROOT_DIR/conan/recipes/portaudio" \
    --version 19.7 \
    --profile:host="$PROF_HOST" \
    --profile:build="$PROF_BUILD" \
    --build=missing

echo "[build] Conan: creating whisper.cpp package (cached after first run)..."
conan create "$ROOT_DIR/conan/recipes/whisper" \
    --version 1.7.4 \
    --profile:host="$PROF_HOST" \
    --profile:build="$PROF_BUILD" \
    "${CONAN_OPTS[@]}" \
    --build=missing

# ---------------------------------------------------------------------------
# Install all Conan dependencies into the build output folder.
# First run for rpi may build gRPC+abseil from source (~30–60 min).
# Subsequent runs use the ~/.conan2 binary cache.
# ---------------------------------------------------------------------------
echo "[build] Conan: installing dependencies..."
conan install "$ROOT_DIR" \
    --profile:host="$PROF_HOST" \
    --profile:build="$PROF_BUILD" \
    "${CONAN_OPTS[@]}" \
    --build=missing \
    --output-folder="$BUILD_DIR"

# Activate the Conan build environment so that protoc and grpc_cpp_plugin
# (from the Conan package cache) are on PATH when cmake runs find_program.
# shellcheck source=/dev/null
source "$BUILD_DIR/conanbuild.sh"

# ---------------------------------------------------------------------------
# Configure and build
# ---------------------------------------------------------------------------
# Wipe proto-generated sources so protoc always re-runs with the current
# Conan-managed protoc binary. Avoids stale-header errors after upgrades.
rm -rf "$BUILD_DIR/vhal_proto_gen"

cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake" \
    -DCMAKE_PREFIX_PATH="$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGGML_CUDA="$GGML_CUDA" \
    -DGGML_HAILO8="$GGML_HAILO8" \
    -S "$ROOT_DIR"

echo "[build] Building with $JOBS jobs..."
cmake --build "$BUILD_DIR" -j"$JOBS"

echo "[build] Done. Binary: build/${TARGET}/velan"
echo "[build] To deploy, run: ./scripts/deploy_velan.sh --target ${TARGET}"
