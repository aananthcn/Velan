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
#
# build_vhal_core.sh — Build vhal-core for the given target platform.
#
# vhal-core is a Velan dependency that provides the VHAL gRPC server
# (vhal-core) and property-forwarding daemon (vhal-gateway).
# This script drives the Conan + CMake build and stages the install tree
# under vhal-core's build/<target>/install/ ready for deploy_vhal_core.sh.
#
# Usage:
#   ./scripts/build_vhal_core.sh --target <pc|rpi> [-j <jobs>]
#
#   --target pc   Native x86-64 build using the linux-x86 Conan profile.
#   --target rpi  Cross-compiled AArch64 build using the rpi5-linux profile.
#                 Requires aarch64-linux-gnu-gcc to be installed.
#
# Override the vhal-core source directory:
#   VHAL_CORE_DIR=/path/to/vhal-core ./scripts/build_vhal_core.sh --target pc

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Default location of the vhal-core repo (override via env if needed).
VHAL_CORE_DIR="${VHAL_CORE_DIR:-${ROOT_DIR}/../../networking/vhal-core}"

TARGET=""
JOBS=$(nproc)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
info()    { echo "[build-vhal] $*"; }
fail()    { echo "[build-vhal] ERROR: $*" >&2; exit 1; }

usage() {
    echo "Usage: $(basename "$0") --target <pc|rpi> [-j <jobs>]"
    echo
    echo "  --target <pc|rpi>   Build target  [required]"
    echo "    pc                Native x86-64 (linux-x86 Conan profile)"
    echo "    rpi               Cross-compiled AArch64 (rpi5-linux Conan profile)"
    echo "                      Requires: aarch64-linux-gnu-gcc"
    echo "  -j <jobs>           Parallel jobs (default: $(nproc))"
    echo
    echo "  Override source path: VHAL_CORE_DIR=/path/to/vhal-core $0 --target pc"
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)
            TARGET="$2"
            if [[ "$TARGET" != "pc" && "$TARGET" != "rpi" ]]; then
                echo "Error: --target must be 'pc' or 'rpi', got '$TARGET'"
                usage; exit 1
            fi
            shift 2 ;;
        -j)
            JOBS="$2"; shift 2 ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            fail "Unknown argument: $1" ;;
    esac
done

[[ -z "$TARGET" ]] && { echo "Error: --target is required"; usage; exit 1; }

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
VHAL_CORE_DIR="$(cd "$VHAL_CORE_DIR" 2>/dev/null || \
    fail "vhal-core source not found at ${VHAL_CORE_DIR}\n       Set VHAL_CORE_DIR=/path/to/vhal-core"
    pwd)"

BUILD_DIR="${VHAL_CORE_DIR}/build/${TARGET}"
INSTALL_DIR="${BUILD_DIR}/install"

# ---------------------------------------------------------------------------
# Conan profile selection
# ---------------------------------------------------------------------------
case "$TARGET" in
    pc)
        CONAN_PROFILE_BUILD="${VHAL_CORE_DIR}/profiles/linux-x86"
        CONAN_PROFILE_HOST="${VHAL_CORE_DIR}/profiles/linux-x86"
        ;;
    rpi)
        CONAN_PROFILE_BUILD="${VHAL_CORE_DIR}/profiles/linux-x86"
        CONAN_PROFILE_HOST="${VHAL_CORE_DIR}/profiles/rpi5-linux"
        # Verify the cross-compiler is available
        command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || \
            fail "Cross-compiler aarch64-linux-gnu-gcc not found.\n"\
                 "       Install with: sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu"
        ;;
esac

# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------
command -v conan >/dev/null 2>&1 || \
    fail "conan not found. Install with: pip3 install conan"

command -v cmake >/dev/null 2>&1 || \
    fail "cmake not found. Install with: sudo apt install cmake"

info "Source  : ${VHAL_CORE_DIR}"
info "Target  : ${TARGET}"
info "Build   : ${BUILD_DIR}"
info "Install : ${INSTALL_DIR}"

# ---------------------------------------------------------------------------
# Conan — install dependencies
# ---------------------------------------------------------------------------
info "Installing Conan dependencies..."
conan install "${VHAL_CORE_DIR}" \
    --profile:build="${CONAN_PROFILE_BUILD}" \
    --profile:host="${CONAN_PROFILE_HOST}" \
    --output-folder="${BUILD_DIR}" \
    --build=missing

# ---------------------------------------------------------------------------
# Locate Conan-generated files
#
# vhal-core's conanfile.py sets self.folders.generators = "build/Release",
# which Conan appends inside --output-folder, giving:
#   build/<target>/build/Release/conan_toolchain.cmake
# Use `find` so the script stays correct if the layout ever changes.
# ---------------------------------------------------------------------------
TOOLCHAIN=$(find "${BUILD_DIR}" -name "conan_toolchain.cmake" 2>/dev/null | head -1)
CONANBUILD=$(find "${BUILD_DIR}" -name "conanbuild.sh"        2>/dev/null | head -1)

[[ -n "$TOOLCHAIN" ]] || \
    fail "conan_toolchain.cmake not found under ${BUILD_DIR} — conan install may have failed."
[[ -n "$CONANBUILD" ]] || \
    fail "conanbuild.sh not found under ${BUILD_DIR} — conan install may have failed."

info "Toolchain : ${TOOLCHAIN}"
info "Conan env : ${CONANBUILD}"

# ---------------------------------------------------------------------------
# CMake — configure
#
# Source conanbuild.sh first so CC, CXX, and STRIP are set to the
# cross-compiler (rpi) or the host compiler (pc) from the Conan profile's
# [buildenv] section.  Without this the cross-compile has no compiler.
# ---------------------------------------------------------------------------
# shellcheck source=/dev/null
source "${CONANBUILD}"

# ---------------------------------------------------------------------------
# Cross-compilation: prepend native (x86_64) code-gen tools to PATH
#
# When cross-compiling for rpi, protobuf::protoc and grpc_cpp_plugin in the
# Conan toolchain point to AArch64 binaries that cannot run on the build
# machine.  protobuf-conan-protoc-target.cmake does:
#   find_program(protoc PATHS ENV PATH NO_DEFAULT_PATH)   # when cross-compiling
# so whatever protoc is first on PATH wins.  Conan also built native x86_64
# versions as tool_requires in the Conan cache — we locate and prepend them
# so CMake picks the right protoc/grpc_cpp_plugin for proto code generation.
# Without this, CMake falls back to the system protoc (3.12.x) which emits
# PROTOBUF_NAMESPACE_ID-style code incompatible with protobuf 5.29.x.
# ---------------------------------------------------------------------------
if [[ "$TARGET" == "rpi" ]]; then
    info "Locating native (x86_64) code-gen tools for cross-compilation..."

    # Returns 0 if the binary can actually execute on this machine.
    can_exec() { "$1" --version >/dev/null 2>&1; local rc=$?; [[ $rc -lt 126 ]]; }

    NATIVE_PATHS=()

    # Native protoc — must be executable and report "libprotoc 29." (protobuf 5.29.x)
    while IFS= read -r p; do
        can_exec "$p" || continue
        "$p" --version 2>/dev/null | grep -q "^libprotoc 29\." || continue
        NATIVE_PATHS+=("$(dirname "$p")")
        info "  protoc        : $p ($("$p" --version 2>/dev/null))"
        break
    done < <(find ~/.conan2/p/b -name "protoc" -path "*/p/bin/protoc" 2>/dev/null)

    # Native grpc_cpp_plugin — must be executable on this machine
    while IFS= read -r p; do
        can_exec "$p" || continue
        NATIVE_PATHS+=("$(dirname "$p")")
        info "  grpc_cpp_plugin: $p"
        break
    done < <(find ~/.conan2/p/b -name "grpc_cpp_plugin" -path "*/p/bin/grpc_cpp_plugin" 2>/dev/null)

    if [[ ${#NATIVE_PATHS[@]} -eq 0 ]]; then
        fail "Native build tools (protoc, grpc_cpp_plugin) not found in Conan cache.\n" \
             "       Run conan install with --build=missing to build them first."
    fi

    PREPEND=$(IFS=:; echo "${NATIVE_PATHS[*]}")
    export PATH="${PREPEND}:${PATH}"
    info "Prepended native tool paths to PATH."
fi

# ---------------------------------------------------------------------------
# Purge stale CMake state before configuring
#
# CMakeCache.txt may have Protobuf_PROTOC_EXECUTABLE cached pointing to the
# system protoc (3.12.x) from a previous failed run.  The generated/ tree may
# hold .pb.cc/.pb.h files produced by that wrong protoc.  Both must be removed
# so CMake picks up the correct native protoc from PATH and regenerates cleanly.
# ---------------------------------------------------------------------------
if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    info "Removing stale CMakeCache.txt..."
    rm -f "${BUILD_DIR}/CMakeCache.txt"
fi
if [[ -d "${BUILD_DIR}/generated" ]]; then
    info "Removing stale generated proto files..."
    rm -rf "${BUILD_DIR}/generated"
fi

info "Configuring..."
cmake -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
    -DCMAKE_BUILD_TYPE=Release \
    -S "${VHAL_CORE_DIR}"

# ---------------------------------------------------------------------------
# CMake — build
# ---------------------------------------------------------------------------
info "Building with ${JOBS} jobs..."
cmake --build "${BUILD_DIR}" -j"${JOBS}"

# ---------------------------------------------------------------------------
# CMake — stage install tree
# ---------------------------------------------------------------------------
info "Staging install tree to ${INSTALL_DIR}..."
cmake --install "${BUILD_DIR}" --prefix "${INSTALL_DIR}"

echo
info "Done."
echo "  Staged binaries : ${INSTALL_DIR}/bin/"
echo "  Staged configs  : ${INSTALL_DIR}/etc/"
echo "  Staged libs     : ${INSTALL_DIR}/lib/"
echo
echo "  Next step: ./scripts/deploy_vhal_core.sh --target ${TARGET}"
