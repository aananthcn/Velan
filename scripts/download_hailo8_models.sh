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

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
STT_DIR="$ROOT_DIR/models/stt"

HAILO_BASE="https://hailo-csdata.s3.eu-west-2.amazonaws.com/resources"

echo "[download] Hailo-8L Whisper model downloader"

# ---------------------------------------------------------------------------
# Helper — skips the file if it already exists (same as download_models.sh).
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

# ---------------------------------------------------------------------------
# Whisper Base (h8l)
# ---------------------------------------------------------------------------
echo "[download] --- Whisper Base ---"
mkdir -p "$STT_DIR/whisper-base-h8l/weights"

download \
    "$HAILO_BASE/whisper/h8l/base-whisper-encoder-5s_h8l.hef" \
    "$STT_DIR/whisper-base-h8l/encoder.hef" \
    "whisper-base encoder HEF"

download \
    "$HAILO_BASE/whisper/h8l/base-whisper-decoder-fixed-sequence-matmul-split_h8l.hef" \
    "$STT_DIR/whisper-base-h8l/decoder.hef" \
    "whisper-base decoder HEF"

download \
    "$HAILO_BASE/npy%20files/whisper/decoder_assets/base/decoder_tokenization/token_embedding_weight_base.npy" \
    "$STT_DIR/whisper-base-h8l/weights/token_embedding_weight_base.npy" \
    "whisper-base token embedding weights"

download \
    "$HAILO_BASE/npy%20files/whisper/decoder_assets/base/decoder_tokenization/onnx_add_input_base.npy" \
    "$STT_DIR/whisper-base-h8l/weights/onnx_add_input_base.npy" \
    "whisper-base positional embedding weights"

# ---------------------------------------------------------------------------
# Whisper Tiny (h8l)
# ---------------------------------------------------------------------------
echo "[download] --- Whisper Tiny ---"
mkdir -p "$STT_DIR/whisper-tiny-h8l/weights"

download \
    "$HAILO_BASE/whisper/h8l/tiny-whisper-encoder-10s_15dB_h8l.hef" \
    "$STT_DIR/whisper-tiny-h8l/encoder.hef" \
    "whisper-tiny encoder HEF"

download \
    "$HAILO_BASE/whisper/h8l/tiny-whisper-decoder-fixed-sequence-matmul-split_h8l.hef" \
    "$STT_DIR/whisper-tiny-h8l/decoder.hef" \
    "whisper-tiny decoder HEF"

download \
    "$HAILO_BASE/npy%20files/whisper/decoder_assets/tiny/decoder_tokenization/token_embedding_weight_tiny.npy" \
    "$STT_DIR/whisper-tiny-h8l/weights/token_embedding_weight_tiny.npy" \
    "whisper-tiny token embedding weights"

download \
    "$HAILO_BASE/npy%20files/whisper/decoder_assets/tiny/decoder_tokenization/onnx_add_input_tiny.npy" \
    "$STT_DIR/whisper-tiny-h8l/weights/onnx_add_input_tiny.npy" \
    "whisper-tiny positional embedding weights"

echo "[download] All Hailo-8L models ready."