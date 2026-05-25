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
# sync_rpi_sysroot.sh — Install arm64 dev packages on the RPi and sync
#   /usr and /lib to ~/sdk/rpi/adas on this machine.
#
# Run once (or after apt upgrades on the RPi) before:
#   ./scripts/build_velan.sh --target rpi
#
# Usage:
#   ./scripts/sync_rpi_sysroot.sh [--ip <addr>] [--user <username>]

set -euo pipefail

SYSROOT_DIR="${HOME}/sdk/rpi/adas"
RPI_IP="192.168.10.30"
RPI_USER="${USER}"

usage() {
    echo "Usage: $(basename "$0") [--ip <addr>] [--user <username>]"
    echo "  --ip <addr>          RPi IP address (default: 192.168.10.30)"
    echo "  --user <username>    RPi login username (default: \$USER)"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ip)      RPI_IP="$2";   shift 2 ;;
        --user)    RPI_USER="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1"; usage; exit 1 ;;
    esac
done

RPI_DEST="${RPI_USER}@${RPI_IP}"

echo "[sdk] Target   : ${RPI_DEST}"
echo "[sdk] Sysroot  : ${SYSROOT_DIR}"

# ---------------------------------------------------------------------------
# SSH connectivity check
# ---------------------------------------------------------------------------
echo "[sdk] Checking SSH connectivity..."
ssh -o ConnectTimeout=5 -o BatchMode=yes "${RPI_DEST}" "exit" 2>/dev/null || {
    echo "[sdk] ERROR: Cannot reach ${RPI_DEST} via SSH."
    echo "[sdk]        Ensure the RPi is online and your public key is authorised."
    exit 1
}
echo "[sdk] SSH OK."

# ---------------------------------------------------------------------------
# Install dev packages on the RPi
# ---------------------------------------------------------------------------
DEV_PKGS=(
    portaudio19-dev
    libasound2-dev      # ALSA — required by portaudio cross-build (PA_USE_ALSA)
    libcurl4-openssl-dev
    libgrpc++-dev
    libprotobuf-dev
)

echo "[sdk] Installing dev packages on RPi..."
ssh -t "${RPI_DEST}" "sudo apt-get install -y ${DEV_PKGS[*]}"
echo "[sdk] Dev packages ready on RPi."

# ---------------------------------------------------------------------------
# Create local sysroot directory (inside $HOME — no sudo needed)
# ---------------------------------------------------------------------------
mkdir -p "${SYSROOT_DIR}"

# ---------------------------------------------------------------------------
# Sync /usr and /lib from RPi
#
# --rsync-path='sudo rsync' runs rsync as root on the remote so all files
# (including protected system paths) can be read intact.
# ---------------------------------------------------------------------------
rsync_from_rpi() {
    local src="$1"
    echo "[sdk] Syncing ${RPI_DEST}:${src} ..."
    rsync -avz --delete \
        --rsync-path='sudo rsync' \
        "${RPI_DEST}:${src}" "${SYSROOT_DIR}"
}

rsync_from_rpi "/usr"
rsync_from_rpi "/lib"

# ---------------------------------------------------------------------------
# Fix absolute symlinks — rsync preserves them as-is, but on the PC they
# point to paths that don't exist (e.g. /usr/lib/libhailort.so →
# /usr/lib/libhailort.so.4.23.0 resolves fine on RPi but is broken here).
# Rewrite any absolute symlink whose target lands inside the sysroot to a
# relative symlink so the cross-compiler and CMake can follow it.
# ---------------------------------------------------------------------------
echo "[sdk] Fixing absolute symlinks in sysroot..."
find "${SYSROOT_DIR}" -type l | while read -r link; do
    target=$(readlink "$link")
    # Skip relative symlinks — they're already correct
    [[ "$target" == /* ]] || continue
    # Absolute path: check if the target exists inside the sysroot
    sysroot_target="${SYSROOT_DIR}${target}"
    [[ -e "$sysroot_target" ]] || continue
    # Rewrite to a relative symlink
    rel=$(realpath --relative-to="$(dirname "$link")" "$sysroot_target")
    ln -sfn "$rel" "$link"
done
echo "[sdk] Symlinks fixed."

echo ""
echo "[sdk] Sysroot sync complete: ${SYSROOT_DIR}"
echo "[sdk] Now run: ./scripts/build_velan.sh --target rpi"
