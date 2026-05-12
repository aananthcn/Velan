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
#   ./scripts/run_velan.sh [VELAN OPTIONS]
#
# Starts vhal-core and velan together.  Ctrl-C (or SIGTERM) stops both.
# All arguments are forwarded to velan (e.g. --sttmodel, --wwphrase, ...).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

VHAL_CORE="${VHAL_CORE:-/opt/car-ui/bin/vhal-core}"
VELAN="$ROOT_DIR/build/velan"

# Make piper (installed by download_models.sh) findable on PATH.
export PATH="$HOME/.local/bin:$PATH"

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
if [[ ! -x "$VHAL_CORE" ]]; then
    echo "[run] ERROR: vhal-core not found at $VHAL_CORE"
    echo "[run]        Set VHAL_CORE=/path/to/vhal-core or install it there."
    exit 1
fi

if [[ ! -x "$VELAN" ]]; then
    echo "[run] ERROR: velan binary not found at $VELAN"
    echo "[run]        Run: ./scripts/build_velan.sh"
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
    [[ -n "$VHAL_PID" ]] && kill "$VHAL_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    echo "[run] Done."
}

trap cleanup INT TERM EXIT

# ---------------------------------------------------------------------------
# Launch
# ---------------------------------------------------------------------------
echo "[run] Starting vhal-core..."
"$VHAL_CORE" &
VHAL_PID=$!

sleep 1

echo "[run] Starting velan..."
"$VELAN" "$@" &
VELAN_PID=$!

# If velan exits for any reason the EXIT trap fires and kills vhal-core too.
wait "$VELAN_PID"
