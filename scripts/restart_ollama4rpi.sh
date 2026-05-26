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
# restart_ollama4rpi.sh — Stop the Ollama systemd service and restart Ollama
# with OLLAMA_HOST=0.0.0.0 so that Velan running on the Raspberry Pi can reach
# it over the LAN.
#
# The systemd service binds to 127.0.0.1 by default; this script starts Ollama
# manually on all interfaces.  It also preserves the CUDA and flash-attention
# settings from the systemd override.
#
# Usage:
#   ./scripts/restart_ollama4rpi.sh [--models-dir <path>]
#
#   --models-dir <path>   Override the Ollama models directory.
#                         Default: /usr/share/ollama/.ollama/models
#                         (the directory used by the systemd 'ollama' user)

set -euo pipefail

MODELS_DIR="/usr/share/ollama/.ollama/models"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --models-dir)
            MODELS_DIR="$2"; shift 2 ;;
        --help)
            sed -n '/^# Usage:/,/^[^#]/{ /^[^#]/d; s/^# \{0,2\}//; p }' "$0"
            exit 0 ;;
        *)
            echo "Unknown argument: $1"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Stop the systemd service (requires sudo)
# ---------------------------------------------------------------------------
echo "[ollama4rpi] Stopping ollama systemd service..."
sudo systemctl stop ollama

# Give the process a moment to release the port
sleep 1

# ---------------------------------------------------------------------------
# Start Ollama manually — bound to all interfaces
# ---------------------------------------------------------------------------
echo "[ollama4rpi] Starting ollama on 0.0.0.0:11434 ..."
echo "[ollama4rpi]   OLLAMA_MODELS : ${MODELS_DIR}"

OLLAMA_HOST=0.0.0.0 \
OLLAMA_MODELS="${MODELS_DIR}" \
CUDA_VISIBLE_DEVICES=0 \
OLLAMA_CUDA_COMPUTE_CAPABILITIES=8.9 \
OLLAMA_FLASH_ATTENTION=1 \
    ollama serve &

OLLAMA_PID=$!
echo "[ollama4rpi] Ollama PID: ${OLLAMA_PID}"

# ---------------------------------------------------------------------------
# Wait for Ollama to be ready
# ---------------------------------------------------------------------------
echo "[ollama4rpi] Waiting for Ollama to become ready..."
for i in $(seq 1 15); do
    if curl -sf http://localhost:11434/ > /dev/null 2>&1; then
        break
    fi
    sleep 1
done

if ! curl -sf http://localhost:11434/ > /dev/null 2>&1; then
    echo "[ollama4rpi] ERROR: Ollama did not start within 15 s."
    exit 1
fi

# ---------------------------------------------------------------------------
# Verify it is listening on all interfaces
# ---------------------------------------------------------------------------
LISTEN=$(ss -tlnp 2>/dev/null | grep ':11434' | awk '{print $4}')
echo "[ollama4rpi] Listening on: ${LISTEN}"
echo "[ollama4rpi] Ready — RPi can now reach Ollama at <this_host_ip>:11434"
echo "[ollama4rpi] To stop: kill ${OLLAMA_PID}"
