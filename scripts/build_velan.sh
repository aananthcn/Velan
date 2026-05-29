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
QT_DIR=""      # Qt prefix for velan-ui; auto-detected from ~/Qt/ if not set
QT_HOST_DIR="" # x86_64 Qt host tools (moc/rcc) needed for rpi cross-compile

usage() {
    echo "Usage: $(basename "$0") --target <pc|rpi> [--aicore <cpu|cuda|hailo8>] [-j <jobs>]"
    echo "  --target <pc|rpi>              Build target: pc (local dev) or rpi (Raspberry Pi)  [required]"
    echo "  --aicore <cpu|cuda|hailo8>     AI accelerator (default: cuda for pc, hailo8 for rpi)"
    echo "  --qt-dir <path>                Qt6 gcc_64 prefix for velan-ui (auto-detected from ~/Qt/)"
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
        --qt-dir) QT_DIR="$2"; shift ;;
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
        command -v aarch64-linux-gnu-gcc-12 >/dev/null 2>&1 || {
            echo "[build] ERROR: aarch64-linux-gnu-gcc-12 not found."
            echo "[build]        Install with: sudo apt install gcc-12-aarch64-linux-gnu g++-12-aarch64-linux-gnu"
            exit 1
        }
        PROF_HOST="$ROOT_DIR/profiles/rpi"
        PROF_BUILD="$ROOT_DIR/profiles/pc"
        ;;
esac

# ---------------------------------------------------------------------------
# Qt6 detection — for velan-ui.
#
# PC target:
#   Looks for the latest gcc_64 install under ~/Qt/ (Qt online installer layout).
#   Override with --qt-dir or the QT_DIR env var.
#
# RPi target (cross-compile):
#   Qt6 cross-compilation requires TWO separate installations:
#   1. Target Qt6 libs (aarch64) — from the synced RPi sysroot.
#      Populated by: ./scripts/sync_rpi_sysroot.sh
#      Location:     ~/sdk/rpi/adas/usr/lib/aarch64-linux-gnu/cmake/Qt6/
#   2. Host Qt6 tools (x86_64: moc, rcc, qmlimportscanner) — from ~/Qt/.
#      Required because the aarch64 tool binaries in the sysroot cannot
#      execute on the build machine.
#   If either is missing, velan-ui is skipped for the RPi build.
# ---------------------------------------------------------------------------

# Helper: find the latest gcc_64 Qt6 install under ~/Qt/
_find_pc_qt6_latest() {
    find "$HOME/Qt" -maxdepth 7 \
        -name "Qt6Config.cmake" -path "*/gcc_64/*" \
        2>/dev/null \
    | sort -Vr | head -1 \
    | sed 's|/lib/cmake/Qt6/Qt6Config.cmake||'
}

# Helper: find the PC Qt6 gcc_64 install whose MINOR version matches $1 (e.g. "6.8").
# Falls back to the latest if no minor-version match is found.
# Qt cross-compilation requires host tools and target libs to share the same
# major.minor — a mismatch (e.g. host 6.11.0 + target 6.8.2) breaks cmake
# because newer Qt introduces internal cmake functions the older version doesn't know.
_find_pc_qt6_matching() {
    local target_minor="$1"   # e.g. "6.8"
    local match
    # Prefer an exact major.minor match (e.g. 6.8.x)
    match=$(find "$HOME/Qt" -maxdepth 7 \
                -name "Qt6Config.cmake" -path "*/gcc_64/*" \
                2>/dev/null \
            | grep "/${target_minor}\." \
            | sort -Vr | head -1 \
            | sed 's|/lib/cmake/Qt6/Qt6Config.cmake||')
    if [[ -n "$match" ]]; then
        echo "$match"
    else
        # No patch-for-patch match; fall back to latest and warn at call site
        _find_pc_qt6_latest
    fi
}

if [[ "$TARGET" == "pc" ]]; then
    if [[ -z "$QT_DIR" ]]; then
        QT_DIR=$(_find_pc_qt6_latest)
    fi
    if [[ -n "$QT_DIR" ]]; then
        echo "[build] Qt6 prefix (pc): ${QT_DIR}"
    else
        echo "[build] Qt6 not found — velan-ui will be skipped (-DBUILD_UI=OFF)"
        echo "[build]   Install Qt via https://www.qt.io/download or pass --qt-dir <path>"
    fi

elif [[ "$TARGET" == "rpi" ]]; then
    _SYSROOT="${HOME}/sdk/rpi/adas"
    _QT_SYSROOT_CMAKE="${_SYSROOT}/usr/lib/aarch64-linux-gnu/cmake"

    if [[ -d "${_QT_SYSROOT_CMAKE}/Qt6" ]]; then
        # Read the sysroot Qt version so we can select a matching PC Qt.
        # Qt cross-compilation requires identical major.minor between host tools
        # and target libs; mismatching (e.g. host 6.11 + target 6.8) causes cmake
        # errors like "Unknown CMake command '_qt_internal_should_include_targets'".
        _QT_SYSROOT_VER=$(grep 'set(PACKAGE_VERSION ' \
            "${_QT_SYSROOT_CMAKE}/Qt6/Qt6ConfigVersionImpl.cmake" 2>/dev/null \
            | grep -o '"[0-9.]*"' | tr -d '"' | head -1)
        _QT_SYSROOT_MINOR=$(echo "${_QT_SYSROOT_VER}" | cut -d. -f1,2)  # e.g. "6.8"

        QT_HOST_DIR=$(_find_pc_qt6_matching "${_QT_SYSROOT_MINOR}")
        if [[ -n "$QT_HOST_DIR" ]]; then
            # QT_HOST_DIR = ~/Qt/6.8.3/gcc_64 → version is the parent dir name
            _HOST_VER=$(basename "$(dirname "$QT_HOST_DIR")")
            if [[ "$_HOST_VER" != "${_QT_SYSROOT_MINOR}"* ]]; then
                echo "[build] WARNING: Qt version mismatch — host ${_HOST_VER}, target ${_QT_SYSROOT_VER}"
                echo "[build]   Install Qt ${_QT_SYSROOT_MINOR}.x via https://www.qt.io/download for a clean cross-build"
            fi
            QT_DIR="${_QT_SYSROOT_CMAKE}"
            echo "[build] Qt6 cross-compile (rpi):"
            echo "[build]   Target libs (aarch64) : ${_QT_SYSROOT_CMAKE}  [Qt ${_QT_SYSROOT_VER}]"
            echo "[build]   Host tools  (x86_64)  : ${QT_HOST_DIR}  [Qt ${_HOST_VER}]"
        else
            echo "[build] Qt6 host tools not found — velan-ui will be skipped"
            echo "[build]   Install Qt ${_QT_SYSROOT_MINOR}.x via https://www.qt.io/download"
        fi
    else
        echo "[build] Qt6 not in RPi sysroot — velan-ui will be skipped"
        echo "[build]   Run: ./scripts/sync_rpi_sysroot.sh  (installs qt6-base-dev on RPi)"
    fi
fi

# ---------------------------------------------------------------------------
# RPi cross-compilation: chain the sysroot toolchain into Conan's generated
# toolchain so that GCC 12 searches sysroot headers (avoiding glibc mismatch).
# The path is resolved here at build time so profiles/rpi stays path-agnostic.
# ---------------------------------------------------------------------------
CONAN_USER_TOOLCHAIN_CONF=()
if [[ "$TARGET" == "rpi" ]]; then
    _TC_FILE="${ROOT_DIR}/profiles/rpi-sysroot-toolchain.cmake"
    if [[ ! -f "$_TC_FILE" ]]; then
        echo "[build] ERROR: RPi sysroot toolchain not found: ${_TC_FILE}"
        exit 1
    fi
    CONAN_USER_TOOLCHAIN_CONF=(--conf \
        "tools.cmake.cmaketoolchain:user_toolchain=[\"${_TC_FILE}\"]")
fi

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

# vhal-proto: VHAL gRPC protobuf definitions from vhal-core.
# Platform-independent header-library (no settings → same binary for pc+rpi).
VHAL_CORE_DIR="${HOME}/labs/networking/vhal-core"
if [[ ! -d "${VHAL_CORE_DIR}/packages/vhal-proto" ]]; then
    echo "[build] ERROR: vhal-core not found at ${VHAL_CORE_DIR}"
    echo "[build]        Clone or symlink vhal-core there, or set VHAL_CORE_DIR."
    exit 1
fi
echo "[build] Conan: creating vhal-proto package (cached after first run)..."
conan create "${VHAL_CORE_DIR}/packages/vhal-proto" \
    --version 1.0 \
    --build=missing

# portaudio: upstream recipe auto-detects JACK from the build host and compiles
# pa_jack.c, but never declares libjack in system_libs — causing linker failures
# when cross-compiling for aarch64.  Our local recipe adds PA_USE_JACK=OFF.
echo "[build] Conan: creating portaudio package (cached after first run)..."
conan create "$ROOT_DIR/conan/recipes/portaudio" \
    --version 19.7 \
    --profile:host="$PROF_HOST" \
    --profile:build="$PROF_BUILD" \
    "${CONAN_USER_TOOLCHAIN_CONF[@]}" \
    --build=missing

echo "[build] Conan: creating whisper.cpp package (cached after first run)..."
conan create "$ROOT_DIR/conan/recipes/whisper" \
    --version 1.7.4 \
    --profile:host="$PROF_HOST" \
    --profile:build="$PROF_BUILD" \
    "${CONAN_OPTS[@]}" \
    "${CONAN_USER_TOOLCHAIN_CONF[@]}" \
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
    "${CONAN_USER_TOOLCHAIN_CONF[@]}" \
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
rm -rf "$BUILD_DIR/vhal_proto_gen" "$BUILD_DIR/ui_proto_gen"

# Build CMAKE_PREFIX_PATH.
#
# PC:  QT_DIR (online-installer prefix) goes first so Qt6 component packages
#      (Core, Quick, QuickControls2...) resolve from a single consistent
#      install, not from the system Qt.  Conan output dir follows.
#
# RPi: QT_HOST_DIR (PC gcc_64 Qt) goes first so Qt6 can locate host tools
#      (moc, rcc, qmlimportscanner) that cannot run from the sysroot.
#      Qt6_DIR (sysroot aarch64 Qt) is set in the sysroot toolchain's CACHE,
#      so find_package(Qt6) resolves there directly — not via PREFIX_PATH.
#      We do NOT add the sysroot cmake dir to CMAKE_PREFIX_PATH: it contains
#      gRPCConfig.cmake which references aarch64 executables (grpc_cpp_plugin)
#      that cannot run on x86_64; CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=NEVER
#      (from the sysroot toolchain) already blocks it anyway.
if [[ -n "$QT_DIR" && "$TARGET" == "pc" ]]; then
    CMAKE_PREFIX="${QT_DIR};${BUILD_DIR}"
    # Wipe CMakeCache.txt if any cached Qt6 component _DIR variable still
    # points to the system path (e.g. /usr/lib/x86_64-linux-gnu/cmake/).
    # Those stale entries take precedence over CMAKE_PREFIX_PATH even with
    # NO_DEFAULT_PATH, causing Qt6Config.cmake to mix versions.
    if grep -qs "Qt6.*_DIR.*=/usr/lib" "$BUILD_DIR/CMakeCache.txt"; then
        echo "[build] Stale system Qt dirs in CMakeCache.txt — wiping to use Qt ${QT_DIR}"
        rm -f "$BUILD_DIR/CMakeCache.txt"
    fi
elif [[ -n "$QT_HOST_DIR" && "$TARGET" == "rpi" ]]; then
    CMAKE_PREFIX="${QT_HOST_DIR};${BUILD_DIR}"
else
    CMAKE_PREFIX="${BUILD_DIR}"
fi

# Extra cmake args.
#
# QT_HOST_PATH supplies the x86_64 Qt host tools (moc, rcc, qmlimportscanner):
#   PC:  QT_DIR is both the target Qt and the host tools — no separate path needed.
#        QT_HOST_DIR is not set on pc; QT_DIR already covers host tools.
#   RPi: QT_HOST_DIR is the PC gcc_64 Qt (host tools); the aarch64 target Qt is
#        resolved via Qt6_DIR set in profiles/rpi-sysroot-toolchain.cmake.
#
# All other cross-compilation cmake variables (CMAKE_SYSROOT, --sysroot compiler
# flags, FIND_ROOT_PATH_MODE_PROGRAM/PACKAGE NEVER, Qt6_DIR) are set inside
# profiles/rpi-sysroot-toolchain.cmake which is chained via CONAN_USER_TOOLCHAIN_CONF.
CMAKE_EXTRA=()
if [[ -n "$QT_HOST_DIR" ]]; then
    CMAKE_EXTRA+=("-DQT_HOST_PATH=${QT_HOST_DIR}")
    # QT_HOST_PATH_CMAKE_DIR must be set explicitly when QT_HOST_PATH differs
    # from the Qt version installed in QT_ADDITIONAL_PACKAGES_PREFIX_PATH.
    # Without it, Qt6Config.cmake derives the host cmake dir from Qt6HostInfo_DIR
    # which may resolve to a different (newer) Qt version registered on this
    # machine — causing a "Unknown CMake command '_qt_internal_should_include_targets'"
    # error when the wrong host Qt6CoreTools is loaded for the target Qt6Core.
    CMAKE_EXTRA+=("-DQT_HOST_PATH_CMAKE_DIR=${QT_HOST_DIR}/lib/cmake")
fi

cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake" \
    -DCMAKE_PREFIX_PATH="${CMAKE_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGGML_CUDA="$GGML_CUDA" \
    -DGGML_HAILO8="$GGML_HAILO8" \
    "${CMAKE_EXTRA[@]}" \
    -S "$ROOT_DIR"

echo "[build] Building with $JOBS jobs..."
cmake --build "$BUILD_DIR" -j"$JOBS"

echo "[build] Done. Binary: build/${TARGET}/velan"
echo "[build] To deploy, run: ./scripts/deploy_velan.sh --target ${TARGET}"
