#!/usr/bin/env bash
# Copyright 2026 Aananth C N
# Licensed under the Apache License, Version 2.0 (the "License").
#
# test_hailo_wav.sh — Test HailoTranscriber with a known speech WAV file.
#
# Workflow:
#   1. Generate a WAV file locally using Python TTS (pyttsx3/gtts) or piper,
#      then convert to 16 kHz mono 16-bit PCM with sox.
#   2. Copy the WAV to the Raspberry Pi.
#   3. Run: velan --aicore hailo8 --test-wav /tmp/velan_test.wav
#   4. Print the transcription.
#
# Requirements (local machine):
#   sox        — audio format conversion  (sudo apt install sox)
#   piper      — TTS (optional; used if found at ~/.local/lib/piper/piper)
#
# Requirements (RPi):
#   velan binary already deployed at /opt/car-ui/bin/velan
#   HEF files and weights at /opt/car-ui/models/stt/whisper-tiny-h8l/
#
# Usage:
#   ./scripts/test_hailo_wav.sh [--ip <addr>] [--user <name>] [--text "phrase"]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

RPI_IP="192.168.10.30"
RPI_USER="${USER}"
TEST_TEXT="Hello Velan"
TMP_WAV_RAW="/tmp/velan_test_raw.wav"
TMP_WAV_16K="/tmp/velan_test.wav"

info()    { echo "[INFO]  $*"; }
success() { echo "[OK]    $*"; }
fail()    { echo "[ERROR] $*" >&2; exit 1; }

usage() {
    echo "Usage: $(basename "$0") [--ip <addr>] [--user <name>] [--text \"phrase\"]"
    echo
    echo "  --ip   <addr>   RPi IP (default: 192.168.10.30)"
    echo "  --user <name>   RPi SSH user (default: \$USER)"
    echo "  --text <phrase> Text to synthesise and transcribe (default: 'Hello Velan')"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ip)   RPI_IP="$2";   shift 2 ;;
        --user) RPI_USER="$2"; shift 2 ;;
        --text) TEST_TEXT="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "Unknown arg: $1"; usage ;;
    esac
done

RPI_DEST="${RPI_USER}@${RPI_IP}"

# ---------------------------------------------------------------------------
# Step 1: Generate speech WAV locally
# ---------------------------------------------------------------------------
info "Generating speech WAV for: \"${TEST_TEXT}\""

PIPER_BIN="${HOME}/.local/lib/piper/piper"
TTS_MODEL="${ROOT_DIR}/models/tts/en_US-lessac-medium.onnx"

if [[ -x "${PIPER_BIN}" && -f "${TTS_MODEL}" ]]; then
    info "Using piper TTS → ${TMP_WAV_RAW}"
    echo "${TEST_TEXT}" | "${PIPER_BIN}" \
        --model "${TTS_MODEL}" \
        --output_file "${TMP_WAV_RAW}" \
        --sentence_silence 0.3
    success "Piper TTS done."
elif command -v python3 >/dev/null 2>&1; then
    info "Piper not found — generating sine-tone WAV via Python"
    python3 - "${TMP_WAV_RAW}" <<'PYEOF'
import sys, wave, struct, math
path = sys.argv[1]
# 1 second of 880 Hz sine at 22050 Hz to stand in for speech
sr, dur, freq = 22050, 1.0, 880.0
n = int(sr * dur)
samples = [int(32767 * math.sin(2 * math.pi * freq * i / sr)) for i in range(n)]
with wave.open(path, 'w') as wf:
    wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(sr)
    wf.writeframes(struct.pack(f'<{n}h', *samples))
print(f"  wrote {n} samples to {path}")
PYEOF
    info "NOTE: Using sine-tone fallback — transcription result will be meaningless for"
    info "      language content but will validate that the decoder runs without crashes."
else
    fail "Neither piper nor python3 found. Install one to generate test audio."
fi

# ---------------------------------------------------------------------------
# Step 2: Resample to 16 kHz mono 16-bit PCM
# ---------------------------------------------------------------------------
if command -v sox >/dev/null 2>&1; then
    info "Resampling to 16 kHz mono 16-bit PCM → ${TMP_WAV_16K}"
    sox "${TMP_WAV_RAW}" -r 16000 -c 1 -b 16 "${TMP_WAV_16K}"
    success "Resample done. $(soxi -s "${TMP_WAV_16K}") samples ($(soxi -D "${TMP_WAV_16K}")s)"
elif command -v ffmpeg >/dev/null 2>&1; then
    info "Resampling with ffmpeg → ${TMP_WAV_16K}"
    ffmpeg -y -i "${TMP_WAV_RAW}" -ar 16000 -ac 1 -sample_fmt s16 "${TMP_WAV_16K}" 2>/dev/null
    success "Resample done."
else
    info "sox/ffmpeg not found — copying raw WAV (may not be 16 kHz)"
    cp "${TMP_WAV_RAW}" "${TMP_WAV_16K}"
fi

# ---------------------------------------------------------------------------
# Step 3: Copy WAV to RPi
# ---------------------------------------------------------------------------
info "Copying WAV to ${RPI_DEST}:/tmp/velan_test.wav ..."
scp "${TMP_WAV_16K}" "${RPI_DEST}:/tmp/velan_test.wav"
success "Copy done."

# ---------------------------------------------------------------------------
# Step 4: Run velan --test-wav on the RPi
# ---------------------------------------------------------------------------
VELAN_BIN="/opt/car-ui/bin/velan"
ENCODER_HEF="/opt/car-ui/models/stt/whisper-tiny-h8l/encoder.hef"
DECODER_HEF="/opt/car-ui/models/stt/whisper-tiny-h8l/decoder.hef"
VOCAB_JSON="/opt/car-ui/models/stt/whisper-tiny-h8l/weights/vocab.json"

echo
info "Running transcription on ${RPI_DEST} ..."
echo "  Command: ${VELAN_BIN} --aicore hailo8 --encoder-hef ${ENCODER_HEF} --decoder-hef ${DECODER_HEF} --vocab-json ${VOCAB_JSON} --test-wav /tmp/velan_test.wav"
echo

ssh "${RPI_DEST}" \
    "${VELAN_BIN} --aicore hailo8 \
        --encoder-hef '${ENCODER_HEF}' \
        --decoder-hef '${DECODER_HEF}' \
        --vocab-json  '${VOCAB_JSON}' \
        --test-wav /tmp/velan_test.wav 2>&1"

echo
success "Test complete."
echo
echo "Expected: transcription of \"${TEST_TEXT}\" (or similar)"
echo "If you see garbage output, the mel spectrogram or decoder is still wrong."
echo "If you see the correct text (or close), the N_FFT=400 fix is working!"
