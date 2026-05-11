#!/usr/bin/env bash

VHAL_CORE=/opt/car-ui/bin/vhal-core
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
VELAN="$ROOT_DIR/build/velan"

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

echo "[run] Starting vhal-core..."
"$VHAL_CORE" &
VHAL_PID=$!

sleep 1

echo "[run] Starting velan..."
"$VELAN" "$@" &
VELAN_PID=$!

# If velan exits for any reason, the EXIT trap fires and kills vhal-core too
wait "$VELAN_PID"
