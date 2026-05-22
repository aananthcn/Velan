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
usage() {
    echo "Usage: $(basename "$0") --target <pc|rpi> [--ip <ip>] [--user <username>] [VELAN OPTIONS]"
    echo "  --target <pc|rpi>    Run target: pc (local) or rpi (Raspberry Pi)  [required]"
    echo "  --ip <addr>          RPi IP address (default: 192.168.10.30)        [rpi only]"
    echo "  --user <username>    RPi login username (default: \$USER)            [rpi only]"
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
SSH_PID=

cleanup() {
    echo ""
    echo "[run] Shutting down..."
    [[ -n "$SSH_PID"   ]] && kill "$SSH_PID"   2>/dev/null || true
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
    "$VELAN" "${VELAN_ARGS[@]}" &
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

    echo "[run] Starting velan on RPi (${RPI_DEST})..."
    ssh -t "$RPI_DEST" \
        "export PATH=${DEPLOY_ROOT}/bin:\$PATH; ${DEPLOY_ROOT}/bin/velan ${VELAN_ARGS[*]}" &
    SSH_PID=$!

    wait "$SSH_PID"
fi
