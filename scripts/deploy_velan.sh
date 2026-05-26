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
# deploy_velan.sh — Deploy Velan build outputs to /opt/car-ui
#
# Must be run after build_velan.sh. Copies the velan binary, Piper TTS libs,
# and AI models to /opt/car-ui on the local machine (pc) or a Raspberry Pi
# (rpi) over SSH/rsync.
#
# Usage:
#   ./scripts/deploy_velan.sh --target pc
#   ./scripts/deploy_velan.sh --target rpi [--ip <addr>] [--user <username>]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

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
# Source paths
# ---------------------------------------------------------------------------
VELAN_BIN="${ROOT_DIR}/build/${TARGET}/velan"
# Architecture-specific piper lib directories.
# PC deploys use the native x86_64 install; RPi deploys use the aarch64 tarball
# cached in a separate directory so the two never overwrite each other.
PIPER_LIB_DIR_PC="${HOME}/.local/lib/piper"
PIPER_LIB_DIR_RPI="${HOME}/.local/lib/piper-aarch64"
MODELS_STT="${ROOT_DIR}/models/stt"
MODELS_TTS="${ROOT_DIR}/models/tts"

# ---------------------------------------------------------------------------
# ensure_piper_aarch64 — download the aarch64 piper release if not cached.
#   Stores binaries + shared libs in PIPER_LIB_DIR_RPI so they never collide
#   with the native x86_64 install.
# ---------------------------------------------------------------------------
PIPER_API_URL="https://api.github.com/repos/rhasspy/piper/releases/latest"

ensure_piper_aarch64() {
    if [[ -x "${PIPER_LIB_DIR_RPI}/piper" ]]; then
        info "aarch64 Piper already cached at ${PIPER_LIB_DIR_RPI} — skipping download."
        return
    fi

    local tarball="piper_linux_aarch64.tar.gz"
    local tmp="/tmp/${tarball}"

    info "Resolving latest Piper aarch64 release..."
    local url
    url=$(curl -fsSL "$PIPER_API_URL" \
          | grep -o '"browser_download_url": *"[^"]*'"${tarball}"'"' \
          | grep -o 'https://[^"]*')

    if [[ -z "$url" ]]; then
        fail "Could not resolve Piper aarch64 download URL.\n" \
             "Check https://github.com/rhasspy/piper/releases manually."
    fi

    info "Downloading Piper aarch64 binary..."
    curl -fL -# -o "$tmp" "$url"

    if ! gzip -t "$tmp" 2>/dev/null; then
        rm -f "$tmp"
        fail "Downloaded Piper tarball is not a valid gzip archive."
    fi

    mkdir -p "${PIPER_LIB_DIR_RPI}"
    tar -xzf "$tmp" -C /tmp
    cp -r /tmp/piper/. "${PIPER_LIB_DIR_RPI}/"
    rm -rf "$tmp" /tmp/piper

    success "aarch64 Piper cached → ${PIPER_LIB_DIR_RPI}"
}

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
[[ -x "$VELAN_BIN" ]] || \
    fail "velan binary not found at build/${TARGET}/velan\n       Run: ./scripts/build_velan.sh --target ${TARGET}"

if [[ "$TARGET" == "pc" ]]; then
    [[ -d "$PIPER_LIB_DIR_PC" ]] || \
        fail "Piper libs not found at ${PIPER_LIB_DIR_PC}\n       Run: ./scripts/download_models.sh"
fi

[[ -d "$MODELS_STT" ]] && [[ -n "$(ls -A "$MODELS_STT" 2>/dev/null)" ]] || \
    fail "STT models not found in models/stt/\n       Run: ./scripts/download_models.sh"

[[ -d "$MODELS_TTS" ]] && [[ -n "$(ls -A "$MODELS_TTS" 2>/dev/null)" ]] || \
    fail "TTS models not found in models/tts/\n       Run: ./scripts/download_models.sh"

info "Velan binary : build/${TARGET}/velan"
if [[ "$TARGET" == "rpi" ]]; then
    info "Piper libs   : ${PIPER_LIB_DIR_RPI}  (aarch64)"
else
    info "Piper libs   : ${PIPER_LIB_DIR_PC}  (x86_64)"
fi
info "STT models   : models/stt/"
info "TTS models   : models/tts/"
info "Deploy root  : ${DEPLOY_ROOT}"

# Runtime packages Velan needs on the target machine.
# Note: libcurl4 was renamed to libcurl4t64 in Ubuntu 24.04.
# We check for both — if either is installed the dependency is satisfied.
RUNTIME_DEPS=(
    libportaudio2       # PortAudio — microphone and speaker I/O
    libgomp1            # OpenMP runtime — whisper.cpp multi-threading
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
    # libcurl: Ubuntu 22.04 uses libcurl4; Ubuntu 24.04 renamed it libcurl4t64.
    # Check for either — only add to missing if neither is present.
    local curl_ok=false
    for cpkg in libcurl4t64 libcurl4; do
        if [[ -z "$dest" ]]; then
            dpkg-query -W -f='${Status}' "$cpkg" 2>/dev/null \
                | grep -q "install ok installed" && curl_ok=true && break
        else
            ssh "${dest}" \
                "dpkg-query -W -f='\${Status}' '$cpkg' 2>/dev/null \
                 | grep -q 'install ok installed'" 2>/dev/null \
                && curl_ok=true && break
        fi
    done
    $curl_ok || missing+=("libcurl4t64")

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
# deploy_pc — copy to /opt/car-ui on the local machine
# ---------------------------------------------------------------------------
deploy_pc() {
    local PIPER_LIB_DIR="${PIPER_LIB_DIR_PC}"
    echo
    info "Deploying to pc at ${DEPLOY_ROOT} ..."
    echo "  The following will be written under ${DEPLOY_ROOT}:"
    echo "    bin/velan"
    echo "    bin/piper  (wrapper)"
    echo "    lib/piper/ (Piper binary + shared libs)"
    echo "    models/stt/"
    echo "    models/tts/"
    echo
    read -r -p "Proceed? This requires sudo. [y/N] " CONFIRM
    [[ "${CONFIRM,,}" == "y" ]] || { info "Aborted."; exit 0; }

    install_runtime_deps

    # Ensure directory structure exists
    for dir in "${DEPLOY_ROOT}/bin" "${DEPLOY_ROOT}/lib" \
               "${DEPLOY_ROOT}/models/stt" "${DEPLOY_ROOT}/models/tts"; do
        if [[ ! -d "$dir" ]]; then
            info "Creating $dir ..."
            sudo mkdir -p "$dir"
        fi
    done

    info "Copying velan binary..."
    sudo install -m 0755 "$VELAN_BIN" "${DEPLOY_ROOT}/bin/velan"

    info "Syncing Piper libs (x86_64)..."
    sudo rsync -a --delete "${PIPER_LIB_DIR}/" "${DEPLOY_ROOT}/lib/piper/"

    info "Writing Piper wrapper..."
    sudo tee "${DEPLOY_ROOT}/bin/piper" > /dev/null <<'EOF'
#!/usr/bin/env bash
exec /opt/car-ui/lib/piper/piper "$@"
EOF
    sudo chmod 0755 "${DEPLOY_ROOT}/bin/piper"

    info "Syncing STT models..."
    sudo rsync -a --progress "${MODELS_STT}/" "${DEPLOY_ROOT}/models/stt/"

    info "Syncing TTS models..."
    sudo rsync -a --progress "${MODELS_TTS}/" "${DEPLOY_ROOT}/models/tts/"

    success "Deployed to ${DEPLOY_ROOT}."
    echo
    echo "  Binary   : ${DEPLOY_ROOT}/bin/velan"
    echo "  Piper    : ${DEPLOY_ROOT}/bin/piper  →  lib/piper/"
    echo "  STT      : ${DEPLOY_ROOT}/models/stt/"
    echo "  TTS      : ${DEPLOY_ROOT}/models/tts/"
}

# ---------------------------------------------------------------------------
# deploy_rpi — copy to /opt/car-ui on the Raspberry Pi over SSH/rsync
# ---------------------------------------------------------------------------
deploy_rpi() {
    local RPI_DEST="${RPI_USER}@${RPI_IP}"

    echo
    info "Target RPi   : ${RPI_DEST}"
    info "Deploy root  : ${DEPLOY_ROOT}"

    # Ensure we have the aarch64 piper binary cached locally before touching the RPi
    ensure_piper_aarch64

    # Check SSH connectivity
    info "Checking SSH connectivity to ${RPI_IP} ..."
    if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "${RPI_DEST}" "exit" 2>/dev/null; then
        fail "Cannot reach ${RPI_DEST} via SSH.\n"\
             "Ensure the RPi is online, openssh-server is running, and\n"\
             "your public key is in ~/.ssh/authorized_keys on the device."
    fi
    success "SSH connection OK."

    # Stop the velan service so the binary is not locked during overwrite
    info "Stopping velan.service on RPi (if running) ..."
    ssh -t "${RPI_DEST}" "sudo systemctl stop velan.service 2>/dev/null || true"
    success "velan.service stopped (or was not running)."

    install_runtime_deps "${RPI_DEST}"

    # Ensure /opt/car-ui structure exists on the RPi
    info "Ensuring ${DEPLOY_ROOT} exists on RPi ..."
    ssh -t "${RPI_DEST}" \
        "sudo mkdir -p ${DEPLOY_ROOT}/bin ${DEPLOY_ROOT}/lib ${DEPLOY_ROOT}/models/stt ${DEPLOY_ROOT}/models/tts && \
         sudo chown -R ${RPI_USER}:${RPI_USER} ${DEPLOY_ROOT}"

    echo
    echo "  The following will be written to ${RPI_DEST}:${DEPLOY_ROOT}:"
    echo "    bin/velan"
    echo "    bin/piper  (wrapper, aarch64)"
    echo "    lib/piper/ (Piper aarch64 binary + shared libs)"
    echo "    models/stt/"
    echo "    models/tts/"
    echo
    read -r -p "Proceed? [y/N] " CONFIRM
    [[ "${CONFIRM,,}" == "y" ]] || { info "Aborted."; exit 0; }

    info "Copying velan binary..."
    scp "$VELAN_BIN" "${RPI_DEST}:${DEPLOY_ROOT}/bin/velan"
    ssh "${RPI_DEST}" "chmod 0755 ${DEPLOY_ROOT}/bin/velan"

    info "Syncing Piper libs (aarch64)..."
    rsync -a --delete --progress \
        "${PIPER_LIB_DIR_RPI}/" "${RPI_DEST}:${DEPLOY_ROOT}/lib/piper/"

    info "Writing Piper wrapper..."
    ssh "${RPI_DEST}" \
        "printf '#!/usr/bin/env bash\nexec /opt/car-ui/lib/piper/piper \"\$@\"\n' \
         > ${DEPLOY_ROOT}/bin/piper && chmod 0755 ${DEPLOY_ROOT}/bin/piper"

    info "Syncing STT models..."
    rsync -a --progress \
        "${MODELS_STT}/" "${RPI_DEST}:${DEPLOY_ROOT}/models/stt/"

    info "Syncing TTS models..."
    rsync -a --progress \
        "${MODELS_TTS}/" "${RPI_DEST}:${DEPLOY_ROOT}/models/tts/"

    success "Deployed to ${RPI_DEST}:${DEPLOY_ROOT}."
    echo
    echo "  Binary   : ${DEPLOY_ROOT}/bin/velan"
    echo "  Piper    : ${DEPLOY_ROOT}/bin/piper  →  lib/piper/"
    echo "  STT      : ${DEPLOY_ROOT}/models/stt/"
    echo "  TTS      : ${DEPLOY_ROOT}/models/tts/"

    # Restart the service with the newly deployed binaries
    info "Restarting velan.service on RPi ..."
    if ssh -t "${RPI_DEST}" "sudo systemctl start velan.service 2>/dev/null"; then
        success "velan.service restarted."
    else
        warn "velan.service is not installed yet — skipping restart."
        warn "Run './scripts/run_velan.sh --target rpi' to start Velan manually."
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
case "$TARGET" in
    pc)  deploy_pc  ;;
    rpi) deploy_rpi ;;
esac
