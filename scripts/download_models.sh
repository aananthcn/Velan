#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

STT_DIR="$ROOT_DIR/models/stt"
TTS_DIR="$ROOT_DIR/models/tts"

PIPER_LIB_DIR="$HOME/.local/lib/piper"
PIPER_BIN="$HOME/.local/bin/piper"

STT_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin"

TTS_VOICE="en_US-lessac-medium"
TTS_BASE_URL="https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium"

PIPER_API_URL="https://api.github.com/repos/rhasspy/piper/releases/latest"

mkdir -p "$STT_DIR" "$TTS_DIR" "$PIPER_LIB_DIR" "$HOME/.local/bin"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
download() {
    local url="$1"
    local dest="$2"
    local label="$3"

    if [[ -f "$dest" ]]; then
        echo "[download] $label already exists — skipping."
        return
    fi

    echo "[download] Downloading $label..."
    curl -fL -# -o "$dest" "$url"
    echo "[download] $label done."
}

download_piper() {
    if [[ -f "$PIPER_BIN" ]]; then
        echo "[download] Piper already installed — skipping."
        return
    fi

    local arch
    arch=$(uname -m)
    case "$arch" in
        x86_64)  arch_tag="x86_64"  ;;
        aarch64) arch_tag="aarch64" ;;
        armv7l)  arch_tag="armv7l"  ;;
        *)
            echo "[download] Unsupported architecture: $arch — install piper manually."
            return 1
            ;;
    esac

    local tarball="piper_linux_${arch_tag}.tar.gz"
    local tmp="/tmp/$tarball"

    echo "[download] Resolving latest Piper release..."
    local url
    url=$(curl -fsSL "$PIPER_API_URL" \
          | grep -o '"browser_download_url": *"[^"]*'"$tarball"'"' \
          | grep -o 'https://[^"]*')

    if [[ -z "$url" ]]; then
        echo "[download] ERROR: could not resolve Piper download URL for $tarball"
        return 1
    fi

    echo "[download] Downloading Piper binary (${arch_tag})..."
    curl -fL -# -o "$tmp" "$url"

    if ! gzip -t "$tmp" 2>/dev/null; then
        echo "[download] ERROR: downloaded file is not a valid gzip archive"
        rm -f "$tmp"
        return 1
    fi

    echo "[download] Installing Piper to ~/.local/..."
    # Tarball extracts to a piper/ subdirectory — copy contents into PIPER_LIB_DIR
    tar -xzf "$tmp" -C /tmp
    cp -r /tmp/piper/. "$PIPER_LIB_DIR/"
    rm -rf "$tmp" /tmp/piper

    # Wrapper script so the binary finds its bundled shared libs
    cat > "$PIPER_BIN" <<'EOF'
#!/usr/bin/env bash
PIPER_LIB_DIR="$HOME/.local/lib/piper"
export LD_LIBRARY_PATH="$PIPER_LIB_DIR:${LD_LIBRARY_PATH:-}"
exec "$PIPER_LIB_DIR/piper" "$@"
EOF
    chmod +x "$PIPER_BIN"

    echo "[download] Piper installed → $PIPER_BIN"
}

# ---------------------------------------------------------------------------
# Downloads
# ---------------------------------------------------------------------------
download "$STT_URL" \
         "$STT_DIR/ggml-medium.bin" \
         "Whisper medium (STT)"

download "$TTS_BASE_URL/${TTS_VOICE}.onnx" \
         "$TTS_DIR/${TTS_VOICE}.onnx" \
         "Piper voice model (TTS)"

download "$TTS_BASE_URL/${TTS_VOICE}.onnx.json" \
         "$TTS_DIR/${TTS_VOICE}.onnx.json" \
         "Piper voice config (TTS)"

download_piper

echo "[download] All models and binaries ready."
