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
# deploy_vhal_core.sh — Deploy vhal-core build outputs to /opt/car-ui
#
# Must be run after build_vhal_core.sh. Copies the staged install tree
# (vhal-core's build/<target>/install/) to /opt/car-ui on the local machine
# (pc) or a Raspberry Pi (rpi) over SSH/rsync.
#
# Usage:
#   ./scripts/deploy_vhal_core.sh --target pc
#   ./scripts/deploy_vhal_core.sh --target rpi [--ip <addr>] [--user <username>]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

VHAL_CORE_DIR="${VHAL_CORE_DIR:-${ROOT_DIR}/../../networking/vhal-core}"
DEPLOY_ROOT="/opt/car-ui"
RPI_IP="192.168.10.30"
RPI_USER="${USER}"
TARGET=""

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
info()    { echo "[INFO]  $*"; }
success() { echo "[OK]    $*"; }
warn()    { echo "[WARN]  $*"; }
fail()    { echo "[ERROR] $*" >&2; exit 1; }

usage() {
    echo "Usage: $(basename "$0") --target <pc|rpi> [--ip <addr>] [--user <username>]"
    echo
    echo "  --target <pc|rpi>    Deploy target  [required]"
    echo "    pc                 Deploy to /opt/car-ui on this machine (requires sudo)"
    echo "    rpi                Deploy to /opt/car-ui on the Raspberry Pi via rsync+SSH"
    echo "  --ip <addr>          RPi IP address (default: 192.168.10.30)  [rpi only]"
    echo "  --user <username>    RPi login username (default: \$USER)      [rpi only]"
    echo
    echo "  Override source path: VHAL_CORE_DIR=/path/to/vhal-core $0 --target pc"
    exit 1
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) TARGET="$2";   shift 2 ;;
        --ip)     RPI_IP="$2";   shift 2 ;;
        --user)   RPI_USER="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) fail "Unknown argument: $1" ;;
    esac
done

[[ -z "$TARGET" ]] && { echo "Error: --target is required"; usage; }

case "$TARGET" in
    pc|rpi) ;;
    *) fail "Unknown target '$TARGET'. Must be 'pc' or 'rpi'." ;;
esac

# ---------------------------------------------------------------------------
# Resolve staging directory
# ---------------------------------------------------------------------------
VHAL_CORE_DIR="$(cd "$VHAL_CORE_DIR" 2>/dev/null || \
    fail "vhal-core source not found at ${VHAL_CORE_DIR}\n       Set VHAL_CORE_DIR=/path/to/vhal-core"
    pwd)"

STAGE_DIR="${VHAL_CORE_DIR}/build/${TARGET}/install"

# ---------------------------------------------------------------------------
# Pre-flight: verify staging area exists and has content
# ---------------------------------------------------------------------------
[[ -d "$STAGE_DIR" ]] || \
    fail "Staging directory not found: ${STAGE_DIR}\n       Run: ./scripts/build_vhal_core.sh --target ${TARGET}"

[[ -n "$(ls -A "$STAGE_DIR" 2>/dev/null)" ]] || \
    fail "Staging directory is empty: ${STAGE_DIR}\n       Run: ./scripts/build_vhal_core.sh --target ${TARGET}"

info "Staging area : ${STAGE_DIR}"
info "Deploy root  : ${DEPLOY_ROOT}"

# Runtime packages vhal-core needs on the target machine.
# grpc, protobuf, and abseil are statically linked via Conan; only the
# standard C++ runtime and SSL library need to be present on the target.
RUNTIME_DEPS=(
    libstdc++6          # C++ standard library runtime
    libssl3             # OpenSSL — gRPC TLS transport
)

# ---------------------------------------------------------------------------
# install_runtime_deps [ssh-dest]
#   Checks each package first; only calls apt-get for those not yet installed.
#   No argument  → check and install on the local machine (sudo apt-get).
#   With argument → check and install on the remote machine via SSH.
# ---------------------------------------------------------------------------
install_runtime_deps() {
    local dest="${1:-}"
    local pkg missing=()

    info "Checking runtime dependencies..."
    for pkg in "${RUNTIME_DEPS[@]}"; do
        if [[ -z "$dest" ]]; then
            dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null \
                | grep -q "install ok installed" || missing+=("$pkg")
        else
            ssh "${dest}" \
                "dpkg-query -W -f='\${Status}' '$pkg' 2>/dev/null \
                 | grep -q 'install ok installed'" 2>/dev/null \
                || missing+=("$pkg")
        fi
    done

    if [[ ${#missing[@]} -eq 0 ]]; then
        success "All runtime dependencies already installed."
        return
    fi

    info "Installing missing packages: ${missing[*]}"
    if [[ -z "$dest" ]]; then
        sudo apt-get install -y "${missing[@]}"
    else
        ssh -t "${dest}" "sudo apt-get install -y ${missing[*]}"
    fi
    success "Runtime dependencies in place."
}

# ---------------------------------------------------------------------------
# deploy_pc
# ---------------------------------------------------------------------------
deploy_pc() {
    echo
    info "Deploying to pc at ${DEPLOY_ROOT} ..."
    echo "  This will copy the following to ${DEPLOY_ROOT}:"
    find "${STAGE_DIR}" -mindepth 1 -maxdepth 1 | sort | sed 's|^|    |'
    echo
    read -r -p "Proceed? This requires sudo. [y/N] " CONFIRM
    [[ "${CONFIRM,,}" == "y" ]] || { info "Aborted."; exit 0; }

    install_runtime_deps

    for dir in "${DEPLOY_ROOT}/bin" "${DEPLOY_ROOT}/lib" "${DEPLOY_ROOT}/etc"; do
        [[ -d "$dir" ]] || sudo mkdir -p "$dir"
    done

    info "Copying files (sudo rsync)..."
    sudo rsync -av --progress "${STAGE_DIR}/" "${DEPLOY_ROOT}/"

    success "Deployed to ${DEPLOY_ROOT}."
    echo
    echo "  vhal-core  : ${DEPLOY_ROOT}/bin/vhal-core"
    echo "  vhal-gw    : ${DEPLOY_ROOT}/bin/vhal-gateway"
    echo "  Configs    : ${DEPLOY_ROOT}/etc/vhal/"
}

# ---------------------------------------------------------------------------
# deploy_rpi
# ---------------------------------------------------------------------------
deploy_rpi() {
    local RPI_DEST="${RPI_USER}@${RPI_IP}"

    echo
    info "Target RPi   : ${RPI_DEST}"
    info "Deploy root  : ${DEPLOY_ROOT}"

    info "Checking SSH connectivity to ${RPI_IP} ..."
    if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "${RPI_DEST}" "exit" 2>/dev/null; then
        fail "Cannot reach ${RPI_DEST} via SSH.\n"\
             "Ensure the RPi is online, openssh-server is running, and\n"\
             "your public key is in ~/.ssh/authorized_keys on the device."
    fi
    success "SSH connection OK."

    info "Stopping vhal-core service on RPi (if running) ..."
    ssh -t "${RPI_DEST}" \
        "sudo systemctl stop vhal-core.service 2>/dev/null || true; \
         sudo systemctl stop vhal-gateway.service 2>/dev/null || true"
    success "Services stopped (or were not running)."

    install_runtime_deps "${RPI_DEST}"

    info "Ensuring ${DEPLOY_ROOT} exists on RPi ..."
    ssh -t "${RPI_DEST}" \
        "sudo mkdir -p ${DEPLOY_ROOT}/bin ${DEPLOY_ROOT}/lib ${DEPLOY_ROOT}/etc && \
         sudo chown -R ${RPI_USER}:${RPI_USER} ${DEPLOY_ROOT}"

    echo
    echo "  This will rsync the following to ${RPI_DEST}:${DEPLOY_ROOT}:"
    find "${STAGE_DIR}" -mindepth 1 -maxdepth 1 | sort | sed 's|^|    |'
    echo
    read -r -p "Proceed? [y/N] " CONFIRM
    [[ "${CONFIRM,,}" == "y" ]] || { info "Aborted."; exit 0; }

    info "Syncing files to RPi..."
    rsync -av --progress \
        "${STAGE_DIR}/" \
        "${RPI_DEST}:${DEPLOY_ROOT}/"

    success "Deployed to ${RPI_DEST}:${DEPLOY_ROOT}."
    echo
    echo "  vhal-core  : ${DEPLOY_ROOT}/bin/vhal-core"
    echo "  vhal-gw    : ${DEPLOY_ROOT}/bin/vhal-gateway"
    echo "  Configs    : ${DEPLOY_ROOT}/etc/vhal/"

    info "Restarting vhal-core service on RPi ..."
    if ssh -t "${RPI_DEST}" "sudo systemctl start vhal-core.service 2>/dev/null"; then
        success "vhal-core.service restarted."
    else
        warn "vhal-core.service is not installed yet — skipping restart."
        warn "Start manually on the RPi: /opt/car-ui/bin/vhal-core"
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
case "$TARGET" in
    pc)  deploy_pc  ;;
    rpi) deploy_rpi ;;
esac
