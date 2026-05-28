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
UI_PORT="50052"     # gRPC UI server port; empty/0 = disabled
NO_UI=0             # set by --no-ui
UI_TARGET=""        # set by --ui <pc|rpi>; defaults to $TARGET after parsing

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
LLM_HOST=""   # set by --llm <addr>; empty = Ollama runs locally on the RPi

usage() {
    echo "Usage: $(basename "$0") --target <pc|rpi> [--ip <ip>] [--user <username>] [--llm <addr>] [VELAN OPTIONS]"
    echo "  --target <pc|rpi>    Run target: pc (local) or rpi (Raspberry Pi)  [required]"
    echo "  --ui <pc|rpi>        Where to launch velan-ui (default: same as --target)"
    echo "                         pc  — run velan-ui locally on this PC"
    echo "                         rpi — run velan-ui on the RPi via SSH (needs DISPLAY)"
    echo "  --ip <addr>          RPi IP address (default: 192.168.10.30)        [rpi only]"
    echo "  --user <username>    RPi login username (default: \$USER)            [rpi only]"
    echo "  --llm <addr>         Ollama server IP/hostname (default: localhost on RPi) [rpi only]"
    echo "  --tts-sink <device>  ALSA sink for TTS aplay fallback               [rpi only]"
    echo "                       (default: pulse — routes via PulseAudio/PipeWire to BT speaker)"
    echo "  --ui-port <port>     gRPC UI server port on Velan (default: 50052; 0 = disabled)"
    echo "  --no-ui              Do not launch velan-ui (Velan still exposes the port)"
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
        --llm)
            LLM_HOST="$2"; shift 2 ;;
        --ui-port)
            UI_PORT="$2"; shift 2 ;;
        --ui)
            UI_TARGET="$2"
            if [[ "$UI_TARGET" != "pc" && "$UI_TARGET" != "rpi" ]]; then
                echo "Error: --ui must be 'pc' or 'rpi', got '$UI_TARGET'"
                usage; exit 1
            fi
            shift 2 ;;
        --no-ui)
            NO_UI=1; shift ;;
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

# Default --ui to --target when not explicitly set.
UI_TARGET="${UI_TARGET:-$TARGET}"

# --ui rpi only makes sense when velan itself runs on the RPi.
if [[ "$UI_TARGET" == "rpi" && "$TARGET" == "pc" ]]; then
    echo "[run] Warning: --ui rpi ignored when --target pc; using --ui pc."
    UI_TARGET="pc"
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
UI_PID=
_CLEANUP_DONE=0

cleanup() {
    # Guard against double-invocation: the INT/TERM handler fires cleanup()
    # and then the script exits normally, triggering the EXIT handler again.
    [[ "$_CLEANUP_DONE" == "1" ]] && return
    _CLEANUP_DONE=1
    echo ""
    echo "[run] Shutting down..."
    [[ -n "$UI_PID"    ]] && kill "$UI_PID"    2>/dev/null || true
    [[ -n "$VELAN_PID" ]] && kill "$VELAN_PID" 2>/dev/null || true
    [[ -n "$VHAL_PID"  ]] && kill "$VHAL_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    echo "[run] Done."
}

# Helper: start velan-ui locally if the binary exists and --no-ui was not set.
# UI connects to <host>:<UI_PORT> after Velan is ready.
launch_ui() {
    local server_addr="$1"
    if [[ "$NO_UI" == "1" || "$UI_PORT" == "0" ]]; then return; fi
    local VELAN_UI="${DEPLOY_ROOT}/bin/velan-ui"
    if [[ ! -x "$VELAN_UI" ]]; then
        echo "[run] velan-ui not found at ${VELAN_UI} — skipping UI"
        echo "[run]   (build with -DBUILD_UI=ON, then deploy_velan.sh)"
        return
    fi
    echo "[run] Starting velan-ui on PC (server: ${server_addr}, nice=5)..."
    # Run at nice=5 so velan's audio/Whisper threads (nice=0) always win CPU
    # when both processes compete on the same core.
    nice -n 5 "$VELAN_UI" --server "${server_addr}" &
    UI_PID=$!
}

# Launch velan-ui on the RPi via SSH (used when --ui rpi, which is the default
# for --target rpi).  velan-ui connects to localhost:${UI_PORT} because both
# velan and the UI are on the same machine.
#
# The SSH client process (UI_PID) stays alive while velan-ui runs remotely.
# cleanup() kills UI_PID; SSH closing its TCP connection causes the remote
# SSH server to deliver SIGHUP to velan-ui, which Qt handles with a clean exit.
#
# Display environment: DISPLAY=:0 covers the common RPi + HDMI/DSI X11 setup.
# For Wayland set WAYLAND_DISPLAY=wayland-0 (and drop DISPLAY) in your
# /opt/car-ui environment or pass it via --ui-env (not yet implemented).
launch_ui_on_rpi() {
    if [[ "$NO_UI" == "1" || "$UI_PORT" == "0" ]]; then return; fi
    if ! ssh -o ConnectTimeout=5 "${RPI_DEST}" \
             "test -x ${DEPLOY_ROOT}/bin/velan-ui" 2>/dev/null; then
        echo "[run] velan-ui not found at ${DEPLOY_ROOT}/bin/velan-ui on ${RPI_DEST} — skipping UI"
        echo "[run]   (build with -DBUILD_UI=ON, then deploy_velan.sh --target rpi)"
        return
    fi
    echo "[run] Starting velan-ui on RPi (server: localhost:${UI_PORT})..."
    # -tt forces PTY allocation even though stdin is /dev/null.
    # With a PTY, closing the SSH client (via cleanup() → kill $UI_PID) sends
    # SIGHUP to the remote PTY's foreground process group, so velan-ui on the
    # RPi terminates reliably on Ctrl+C from the PC.  Without -tt, killing the
    # local SSH client leaves velan-ui running as an orphan on the RPi.
    # Output is redirected to a log file to avoid mixing with the terminal.
    ssh -tt "${RPI_DEST}" \
        "export PATH=${DEPLOY_ROOT}/bin:\$PATH \
                XDG_RUNTIME_DIR=/run/user/\$(id -u) \
                DISPLAY=:0; \
         exec ${DEPLOY_ROOT}/bin/velan-ui --server localhost:${UI_PORT}" \
        </dev/null >>/tmp/velan-ui-rpi.log 2>&1 &
    UI_PID=$!
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

    # Inject --ui-port so Velan starts its gRPC UI server.
    VELAN_ARGS+=("--ui-port" "${UI_PORT}")

    echo "[run] Starting velan (local)..."
    (cd "$DEPLOY_ROOT" && "$VELAN" "${VELAN_ARGS[@]}") &
    VELAN_PID=$!

    sleep 1
    launch_ui "localhost:${UI_PORT}"

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

    # --- Ollama setup ---
    # Default: Ollama runs locally on the RPi (velan connects to localhost).
    # Override: pass --llm <addr> to point velan at a different machine's Ollama.
    if [[ -n "$LLM_HOST" ]]; then
        # Explicit override — inject and skip RPi setup.
        echo "[run] Ollama host (override): ${LLM_HOST}"
        VELAN_ARGS+=("--llm" "${LLM_HOST}")
    else
        # Default: install, start, and pull on the RPi itself.

        # Which model does velan need?  Honour --llmodel if the caller passed it.
        LLM_MODEL="llama3.2:3b"
        for _i in "${!VELAN_ARGS[@]}"; do
            if [[ "${VELAN_ARGS[$_i]}" == "--llmodel" ]] && \
               (( _i + 1 < ${#VELAN_ARGS[@]} )); then
                LLM_MODEL="${VELAN_ARGS[$(( _i + 1 ))]}"
            fi
        done

        # 1. Install Ollama on RPi if not present.
        #    Use ssh -t to allocate a PTY so the installer's sudo can prompt for a password.
        if ! ssh "${RPI_DEST}" "command -v ollama &>/dev/null" 2>/dev/null; then
            echo "[run] Ollama not found on RPi — installing via official installer (may ask for sudo password)..."
            ssh -t "${RPI_DEST}" "curl -fsSL https://ollama.com/install.sh | sh"
        fi

        # 2. Start Ollama on RPi if not already running.
        if ! ssh "${RPI_DEST}" "ss -tlnp 2>/dev/null | grep -q ':11434'" 2>/dev/null; then
            echo "[run] Starting Ollama on RPi..."
            ssh "${RPI_DEST}" \
                "nohup ollama serve &>/tmp/ollama-velan.log </dev/null & disown"
            echo -n "[run] Waiting for Ollama on RPi..."
            for _i in {1..15}; do
                sleep 1
                if ssh "${RPI_DEST}" \
                       "ss -tlnp 2>/dev/null | grep -q ':11434'" 2>/dev/null; then
                    echo " ready."
                    break
                fi
                echo -n "."
            done
            if ! ssh "${RPI_DEST}" \
                     "ss -tlnp 2>/dev/null | grep -q ':11434'" 2>/dev/null; then
                echo ""
                echo "[run] ERROR: Ollama failed to start on RPi. Check /tmp/ollama-velan.log"
                exit 1
            fi
        fi

        # 3. Pull the required model on the RPi if not already downloaded.
        if ! ssh "${RPI_DEST}" \
                 "ollama list 2>/dev/null | awk 'NR>1{print \$1}' | grep -qx '${LLM_MODEL}'" \
                 2>/dev/null; then
            echo "[run] Pulling model '${LLM_MODEL}' on RPi (this may take a few minutes)..."
            ssh "${RPI_DEST}" "ollama pull ${LLM_MODEL}"
        fi

        echo "[run] Ollama ready on RPi — model: ${LLM_MODEL}."
    fi

    # Inject --ui-port so Velan starts its gRPC UI server on the RPi.
    VELAN_ARGS+=("--ui-port" "${UI_PORT}")

    echo "[run] Starting vhal-core (local)..."
    "$VHAL_CORE" &
    VHAL_PID=$!

    sleep 1

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

    # Launch velan-ui on the chosen target.
    # --ui rpi (default): run on RPi, connects to localhost.
    # --ui pc :           run on this PC, connects to RPi over LAN (old behaviour).
    sleep 1
    if [[ "$UI_TARGET" == "rpi" ]]; then
        launch_ui_on_rpi
    else
        launch_ui "${RPI_IP}:${UI_PORT}"
    fi

    ssh -t "$RPI_DEST" \
        "cd ${DEPLOY_ROOT} && export PATH=${DEPLOY_ROOT}/bin:\$PATH ALSA_CONFIG_DIR=/usr/share/alsa XDG_RUNTIME_DIR=/run/user/\$(id -u); ./bin/velan ${VELAN_ARGS[*]}" || true
    # EXIT trap fires here and kills vhal-core and velan-ui (VHAL_PID, UI_PID).
fi
