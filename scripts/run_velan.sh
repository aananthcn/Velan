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
# Usage:
#   ./scripts/run_velan.sh --target <pc|rpi> [--ip <ip>] [--user <user>] [VELAN OPTIONS]
#
# Runs velan and vhal-core from /opt/car-ui/ on both targets.
# Run deploy_velan.sh first to install the binaries and models there.
#
# --target pc   : start vhal-core and velan from /opt/car-ui/ locally.
#                 Ctrl-C stops both.
# --target rpi  : start vhal-core locally, then SSH into the RPi and run
#                 velan from /opt/car-ui/ there.  Ctrl-C stops all three.
#
# --ip <addr>   : RPi IP address (default: 192.168.10.30)  [rpi only]
# --user <name> : RPi login username (default: $USER)       [rpi only]
#
# Remaining arguments are forwarded to the velan binary.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

VHAL_CORE="${VHAL_CORE:-/opt/car-ui/bin/vhal-core}"

TARGET=""
RPI_IP="192.168.10.30"
RPI_USER="${USER}"
VELAN_ARGS=()

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
LLM_HOST=""   # explicit --llm-host override; empty = auto-detect for rpi

usage() {
    echo "Usage: $(basename "$0") --target <pc|rpi> [--ip <ip>] [--user <username>] [--llm-host <addr>] [VELAN OPTIONS]"
    echo "  --target <pc|rpi>    Run target: pc (local) or rpi (Raspberry Pi)  [required]"
    echo "  --ip <addr>          RPi IP address (default: 192.168.10.30)        [rpi only]"
    echo "  --user <username>    RPi login username (default: \$USER)            [rpi only]"
    echo "  --llm-host <addr>    Ollama server IP/hostname visible from the RPi  [rpi only]"
    echo "                       (default: auto-detect local IP facing the RPi)"
    echo "  --tts-sink <device>  ALSA sink for TTS aplay fallback               [rpi only]"
    echo "                       (default: pulse — routes via PulseAudio/PipeWire to BT speaker)"
    echo "  [VELAN OPTIONS]      Remaining args are forwarded to the velan binary"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)
            TARGET="$2"
            if [[ "$TARGET" != "pc" && "$TARGET" != "rpi" ]]; then
                echo "Error: --target must be 'pc' or 'rpi', got '$TARGET'"
                usage; exit 1
            fi
            shift 2 ;;
        --ip)
            RPI_IP="$2"; shift 2 ;;
        --user)
            RPI_USER="$2"; shift 2 ;;
        --llm-host)
            LLM_HOST="$2"; shift 2 ;;
        --help)
            usage; exit 0 ;;
        *)
            VELAN_ARGS+=("$1"); shift ;;
    esac
done

if [[ -z "$TARGET" ]]; then
    echo "Error: --target is required"
    usage; exit 1
fi

DEPLOY_ROOT="/opt/car-ui"
VELAN="${DEPLOY_ROOT}/bin/velan"

# /opt/car-ui/bin must be on PATH so the deployed piper wrapper is found.
export PATH="${DEPLOY_ROOT}/bin:$PATH"

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
if [[ ! -x "$VHAL_CORE" ]]; then
    echo "[run] ERROR: vhal-core not found at $VHAL_CORE"
    echo "[run]        Set VHAL_CORE=/path/to/vhal-core or install it there."
    exit 1
fi

# For pc: velan must exist locally.
# For rpi: velan lives on the remote machine — checked via SSH in the launch
#          section below after the connection is confirmed.
if [[ "$TARGET" == "pc" ]] && [[ ! -x "$VELAN" ]]; then
    echo "[run] ERROR: velan binary not found at ${DEPLOY_ROOT}/bin/velan"
    echo "[run]        Run: ./scripts/deploy_velan.sh --target pc"
    exit 1
fi

# ---------------------------------------------------------------------------
# Process management
# ---------------------------------------------------------------------------
VHAL_PID=
VELAN_PID=

cleanup() {
    echo ""
    echo "[run] Shutting down..."
    [[ -n "$VELAN_PID" ]] && kill "$VELAN_PID" 2>/dev/null || true
    [[ -n "$VHAL_PID"  ]] && kill "$VHAL_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    echo "[run] Done."
}

trap cleanup INT TERM EXIT

# ---------------------------------------------------------------------------
# Launch — pc
# ---------------------------------------------------------------------------
if [[ "$TARGET" == "pc" ]]; then
    echo "[run] Starting vhal-core (local)..."
    "$VHAL_CORE" &
    VHAL_PID=$!

    sleep 1

    echo "[run] Starting velan (local)..."
    (cd "$DEPLOY_ROOT" && "$VELAN" "${VELAN_ARGS[@]}") &
    VELAN_PID=$!

    wait "$VELAN_PID"
fi

# ---------------------------------------------------------------------------
# Launch — rpi
# ---------------------------------------------------------------------------
if [[ "$TARGET" == "rpi" ]]; then
    RPI_DEST="${RPI_USER}@${RPI_IP}"

    # Verify velan is deployed on the RPi before starting anything locally.
    if ! ssh -o ConnectTimeout=5 "${RPI_DEST}" \
             "test -x ${DEPLOY_ROOT}/bin/velan" 2>/dev/null; then
        echo "[run] ERROR: velan not found at ${DEPLOY_ROOT}/bin/velan on ${RPI_DEST}"
        echo "[run]        Run: ./scripts/deploy_velan.sh --target rpi --ip ${RPI_IP} --user ${RPI_USER}"
        exit 1
    fi

    echo "[run] Starting vhal-core (local)..."
    "$VHAL_CORE" &
    VHAL_PID=$!

    sleep 1

    # --- Inject --llm <host_ip> so velan on the RPi can reach Ollama on this PC ---
    # Only add it if the caller hasn't already passed --llm in VELAN_ARGS.
    if ! printf '%s\n' "${VELAN_ARGS[@]}" | grep -q '^--llm$'; then
        if [[ -z "$LLM_HOST" ]]; then
            # Auto-detect: which local IP does the kernel route toward the RPi?
            LLM_HOST=$(ip route get "${RPI_IP}" 2>/dev/null \
                       | grep -oP 'src \K[\d.]+' | head -1)
        fi
        if [[ -n "$LLM_HOST" ]]; then
            echo "[run] Ollama host (for RPi): ${LLM_HOST}  (override with --llm-host <addr>)"
            VELAN_ARGS+=("--llm" "${LLM_HOST}")
        else
            echo "[run] WARNING: could not detect local IP — velan will try Ollama at localhost."
            echo "[run]          Pass --llm-host <your_pc_ip> if Ollama isn't on the RPi."
        fi
    fi

    # --- Inject --tts-sink pulse for RPi ---
    # Bluetooth (and most RPi audio) is managed by PulseAudio/PipeWire.
    # ALSA's "default" device falls back to dmix which requires hw:0,0 — a card
    # that may not exist on RPi (cards start at 1).  Routing through the "pulse"
    # ALSA plugin bypasses dmix and reaches the BT speaker via PulseAudio.
    # Only inject if the caller hasn't already specified --tts-sink.
    if ! printf '%s\n' "${VELAN_ARGS[@]}" | grep -q '^--tts-sink$'; then
        echo "[run] TTS sink (for RPi): pulse  (override with --tts-sink <alsa-device>)"
        VELAN_ARGS+=("--tts-sink" "pulse")
    fi

    echo "[run] Starting velan on RPi (${RPI_DEST})..."
    # Run SSH in the foreground so the terminal's Ctrl+C propagates through the
    # PTY directly to velan on the RPi, rather than only killing the local SSH client.
    #
    # XDG_RUNTIME_DIR is required so paplay (TTS fallback) can find the
    # PipeWire/PulseAudio socket at /run/user/<uid>/pulse/native.
    # PAM does not set it when SSH executes a command directly (non-login shell),
    # so we derive it from the remote UID at connect time.
    ssh -t "$RPI_DEST" \
        "cd ${DEPLOY_ROOT} && export PATH=${DEPLOY_ROOT}/bin:\$PATH ALSA_CONFIG_DIR=/usr/share/alsa XDG_RUNTIME_DIR=/run/user/\$(id -u); ./bin/velan ${VELAN_ARGS[*]}" || true
    # EXIT trap fires here and kills vhal-core (VHAL_PID).
fi
