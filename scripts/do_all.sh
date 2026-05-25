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
# do_all.sh — build → deploy → run in one command.
#
# Usage:
#   ./scripts/do_all.sh --target <pc|rpi> [OPTIONS] [VELAN OPTIONS]
#
#   --target <pc|rpi>          Required.
#   --aicore <cpu|cuda|hailo8> AI accelerator (default: cuda for pc, hailo8 for rpi).
#   --ip <addr>                RPi IP address (default: 192.168.10.30)  [rpi only]
#   --user <username>          RPi login user  (default: $USER)          [rpi only]
#   -j <jobs>                  Parallel build jobs (default: nproc).
#   [VELAN OPTIONS]            Any remaining args are forwarded to the velan binary.
#
# The deploy step asks for confirmation; this script auto-answers "y".

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

TARGET=""
AICORE=""
RPI_IP="192.168.10.30"
RPI_USER="${USER}"
JOBS=$(nproc)
VELAN_ARGS=()

usage() {
    echo "Usage: $(basename "$0") --target <pc|rpi> [OPTIONS] [VELAN OPTIONS]"
    echo
    echo "  --target <pc|rpi>              Build target  [required]"
    echo "  --aicore <cpu|cuda|hailo8>     AI accelerator (default: cuda for pc, hailo8 for rpi)"
    echo "  --ip <addr>                    RPi IP address (default: 192.168.10.30)  [rpi only]"
    echo "  --user <username>              RPi login user (default: \$USER)          [rpi only]"
    echo "  -j <jobs>                      Parallel build jobs (default: $(nproc))"
    echo "  [VELAN OPTIONS]                Remaining args forwarded to velan"
    echo
    echo "Runs: build_velan.sh → deploy_velan.sh → run_velan.sh"
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
        --aicore)
            AICORE="$2"; shift 2 ;;
        --ip)
            RPI_IP="$2"; shift 2 ;;
        --user)
            RPI_USER="$2"; shift 2 ;;
        -j)
            JOBS="$2"; shift 2 ;;
        --help|-h)
            usage; exit 0 ;;
        *)
            VELAN_ARGS+=("$1"); shift ;;
    esac
done

if [[ -z "$TARGET" ]]; then
    echo "Error: --target is required"
    usage; exit 1
fi

# ---------------------------------------------------------------------------
# Step 1: Build
# ---------------------------------------------------------------------------
echo "[do_all] ── Step 1/3: Build ──────────────────────────────────────────"
BUILD_ARGS=(--target "$TARGET" -j "$JOBS")
[[ -n "$AICORE" ]] && BUILD_ARGS+=(--aicore "$AICORE")
"$SCRIPT_DIR/build_velan.sh" "${BUILD_ARGS[@]}"

# ---------------------------------------------------------------------------
# Step 2: Deploy  (echo "y" auto-answers the single confirmation prompt)
# ---------------------------------------------------------------------------
echo "[do_all] ── Step 2/3: Deploy ─────────────────────────────────────────"
DEPLOY_ARGS=(--target "$TARGET")
if [[ "$TARGET" == "rpi" ]]; then
    DEPLOY_ARGS+=(--ip "$RPI_IP" --user "$RPI_USER")
fi
echo y | "$SCRIPT_DIR/deploy_velan.sh" "${DEPLOY_ARGS[@]}"

# ---------------------------------------------------------------------------
# Step 3: Run
# ---------------------------------------------------------------------------
echo "[do_all] ── Step 3/3: Run ────────────────────────────────────────────"
RUN_ARGS=(--target "$TARGET")
if [[ "$TARGET" == "rpi" ]]; then
    RUN_ARGS+=(--ip "$RPI_IP" --user "$RPI_USER")
fi
"$SCRIPT_DIR/run_velan.sh" "${RUN_ARGS[@]}" "${VELAN_ARGS[@]+"${VELAN_ARGS[@]}"}"
