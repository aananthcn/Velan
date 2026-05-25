# Velan — Architecture & Design Decisions

## What Velan is

Velan is a **voice assistant for automotive** (AAOS/VHAL). It runs on an IVI ECU and provides
wake-word detection, speech-to-text, LLM reasoning, and text-to-speech. It is a **gRPC client**
that polls a VHAL core server for a vendor-defined trigger property.

---

## Pipeline States — strictly mutually exclusive

The system operates in exactly two states. They **never overlap**. This is enforced in code.

```
LISTENING   — WakeWordDetector owns the microphone and runs Whisper inference.
              Speech2TextManager is idle.

PROCESSING  — Speech2TextManager owns the microphone, records, transcribes, queries LLM,
              speaks TTS reply. WakeWordDetector is blocked on a condition_variable.
```

### Why this matters

Both WWD and STT use PortAudio to open the microphone. ALSA in exclusive mode allows only one
stream owner at a time. If both try to open the mic simultaneously, the second one fails silently
or falls back to a hard timeout. The state machine prevents this entirely.

---

## Component responsibilities — strict separation of concerns

### WakeWordDetector
- **Owns**: wake word detection only. Nothing else.
- Listens on the mic using a VAD-gated sliding window.
- Runs `whisper_full()` when a speech utterance is complete.
- On phrase match: closes its mic stream, calls `callback_()`, then **blocks** on a
  `condition_variable` until `resume()` is called.
- `pause()`: called externally (e.g., VHAL trigger) to put WWD into the waiting state.
  WWD checks `stt_active_` at the top of every loop iteration and yields within one chunk (~100 ms).
- `resume()`: called by Speech2TextManager after TTS finishes. Unblocks the detect loop.
- **Does NOT decide when STT stops recording.** End-of-speech is STT's responsibility.

### Speech2TextManager
- **Owns**: the full PROCESSING pipeline — record → transcribe → LLM → TTS.
- `handle_trigger()`: entry point for both VHAL and wake-word triggers.
  1. Calls `on_start_()` → `wwd->pause()` before taking the mic.
  2. Spawns `rec_thread_` which runs: `record_audio()` → `process()` → `is_recording_=false` → `on_done_()`.
  3. `on_done_()` calls `wwd->resume()` → PROCESSING → LISTENING transition.
- `record_audio()` uses internal VAD (silence detection) to stop itself. No external stop signal needed.
- STT is a singleton; `get_context()` exposes the `whisper_context*` for WWD to share.

### TransformerManager
- Owns the Ollama HTTP conversation (model name + multi-turn history).

### Text2SpeechManager
- Owns the Piper TTS subprocess + PortAudio playback.

---

## Shared Whisper context — one model, two uses

WWD and STT **share a single `whisper_context*`** loaded by Speech2TextManager.

**Why**: The tiny model (ggml-tiny.bin, 39M params) was inaccurate for "Subramani" — hallucinated
"Hey Bella", "Hey where are you?" etc. The medium model (ggml-medium.bin, 769M params) is far
more accurate. Sharing is safe because LISTENING and PROCESSING are mutually exclusive — there
is never concurrent access to the context.

**Consequence**: `--wwmodel` / `ggml-tiny.bin` are gone. `--sttmodel` controls both STT and WWD.
Do not re-introduce a separate wake-word model without strong justification.

**Memory**: one model in RAM instead of two (~1.5 GB vs ~1.6 GB).

---

## Trigger sources — both call the same handle_trigger()

```
VHAL gRPC poll (state == 1)  ──┐
                               ├──► Speech2TextManager::handle_trigger()
WakeWordDetector::callback_()  ──┘
```

- VHAL state == 0 is **ignored**. STT manages its own stop via VAD.
- `handle_trigger()` is idempotent — `is_recording_.exchange(true)` guards against double-trigger.

---

## Initialization order in main()

```
1. Declare wwd (unique_ptr, null) — so on_start/on_done lambdas can capture it by ref
2. Construct Speech2TextManager::instance() — loads Whisper model, passes on_start + on_done
3. Construct WakeWordDetector — passes mgr.get_context() (shared context)
4. wwd->start() — WWD begins LISTENING
```

This order is required. WWD needs the context, which only exists after STT is constructed.
The `wwd` null-pointer capture in the lambdas is safe because they are only called after step 4.

---

## State transition callbacks

| Callback | Direction | Caller | Effect |
|----------|-----------|--------|--------|
| `on_start_()` | LISTENING → PROCESSING | `handle_trigger()` | `wwd->pause()` — sets `stt_active_=true` |
| `on_done_()` | PROCESSING → LISTENING | end of `rec_thread_` | `wwd->resume()` — clears `stt_active_`, notifies cv |
| `callback_()` | LISTENING → PROCESSING | `detect_loop()` on phrase match | calls `handle_trigger()` |

---

## Multiple wake phrases

`--wwphrase` accepts a comma-separated list. Any match fires the trigger.

```
./velan --wwphrase "Hey Vela, Subramanya, Subramani, Subrahmanya"
```

Default (`WWD_DEFAULT_WAKE_WORDS` in `WakeWordDetector.h`): `"Hey Vela, Subramanya, Subramani, Subrahmanya"`

Internally each phrase is normalised (lower-case, punctuation → spaces, runs collapsed) and stored
in `wake_phrases_` (vector). `phrase_matches()` checks substring match against all of them.
The `split_phrases()` helper in `main.cpp` handles the comma-split and whitespace trimming.

---

## VAD parameters

### WakeWordDetector (detect_loop)

**Design**: onset-triggered fixed-duration capture (not VAD-based end detection).

Simple threshold VAD is unreliable at the SNR typical in home/car environments: mid-word
phonemes ("ubramanya" after the onset "S") fall below the detection threshold and produce
mostly-silent buffers that Whisper hallucinates on.  The fix: once any chunk exceeds
`eff_threshold`, collect ALL chunks for `LOCK_IN_MS` regardless of per-chunk energy.

- `CHUNK_MS = 100` — mic poll interval; `CHUNK_FRAMES = 1600`
- `LOCK_IN_MS = 1000` / `LOCK_IN_FRAMES = 16000` — fixed capture window after onset; covers the longest expected wake phrase ("Subrahmanya" ≈ 700 ms) with headroom
- `MIN_PHRASE_MS = 200` / `MIN_PHRASE_FRAMES = 3200` — safety floor (always satisfied with 1000 ms capture)
- `WWD_NOISE_CAL_CHUNKS = 5` — calibration duration (~500 ms)
- `WWD_NOISE_MULTIPLIER = 1.2f` — `eff_threshold = median(cal_rms) × 1.2`; capped at `WWD_THRESHOLD_MAX = 0.10f`
- **Median calibration**: individual calibration chunk RMS values are sorted and the median is used as the noise floor estimate. This rejects startup transient spikes (keyboard, AC click) that inflate a mean-based estimate and raise `eff_threshold` too high.
- `audio_ctx = 128` mel frames (~1.28 s) — limits Whisper encoder context for faster CPU inference
- `MIN_SAMPLES = SAMPLE_RATE + CHUNK_FRAMES` (17600, 1100 ms) — padding floor in WhisperTranscriber; whisper's mel-frame formula rounds exactly-1s audio down to 990ms and rejects it

### Speech2TextManager (record_audio)
- `VAD_RMS_THRESHOLD = 0.01f`
- `VAD_MIN_SPEECH_CHUNKS = 5` — ~320 ms of speech must be seen before silence counting starts
- `VAD_SILENCE_CHUNKS = 15` — ~960 ms of sustained silence ends recording
- `VAD_MAX_RECORD_FRAMES = SAMPLE_RATE * 90` — hard cap of 1.5 min; guards against VAD failure due to background noise

---

## test/trigger_velan.py

Sends `SetValues(VOICE_ASSIST_TRIGGER)` to VHAL to simulate a hardware button.

- **ENTER** → `TRIGGER_ON` (state = 1) — starts PROCESSING
- **BACKSPACE or ESC** → `TRIGGER_OFF` (state = 0) — sent for completeness; Velan ignores it
- **Ctrl-C** → quit

This is **not** hold-to-talk. It was changed from the original auto-repeat / release-timeout
pattern to a press-to-start / press-to-stop pattern because STT now self-terminates via VAD.

---

## Phase 2 — Hailo-8L NPU acceleration

`HailoTranscriber` implements `ITranscriber` using the HailoRT C++ InferModel API.
It replaces whisper.cpp CPU/GPU inference entirely with NPU inference on the Hailo-8L.

### Architecture
```
transcribe(pcm)
  ├─ log_mel(pcm)               CPU: FFT + mel filterbank + log normalisation
  ├─ run_encoder(mel)           NPU: encoder.hef (one shot, ~3000 mel frames in)
  ├─ greedy_decode(enc_out):    NPU: decoder.hef (autoregressive until EOT)
  │    for each step:
  │      run_decoder_step(enc_out, tokens) → next_token = argmax(logits)
  └─ detokenise(token_ids)      CPU: BPE vocab.json lookup
```

### Setup: HEF files + matmul-split weights
1. Drop `encoder.hef` and `decoder.hef` in `models/stt/`
2. Run `hailortcli parse-hef models/stt/encoder.hef` — note the **exact stream names**
3. Update `ENCODER_INPUT_NAME`, `ENCODER_OUTPUT_NAME`, `DECODER_INPUT_ENC`,
   `DECODER_INPUT_TOKS`, `DECODER_OUTPUT_NAME` at the top of `src/HailoTranscriber.cpp`
4. Verify mel layout matches encoder input shape — the tiny HEF encoder input is
   `FCR(1×1000×80)` meaning **[frames × mel_bands]**.  `log_mel()` produces
   `[mel_bands × frames]`, so `run_encoder()` transposes before `set_buffer()`.  If you
   swap in a different HEF, re-check this with `hailortcli parse-hef` and update the
   transpose if the shape changes.
5. Check decoder token dtype (float32 or int32 — see TODO in `decoder_step()`)
6. Extract embedding weights, mel filterbank, and `vocab.json` using the extraction script:
   ```bash
   # On any machine with openai-whisper installed:
   python3 scripts/extract_whisper_weights.py --model tiny --out models/stt
   # (use --model base/small/medium to match the model size the HEF was compiled from)
   ```
   This creates:
   - `models/stt/token_embedding_weight_tiny.npy`  — decoder input embedding [vocab × d_model]
   - `models/stt/onnx_add_input_tiny.npy`           — positional encoding [max_ctx × d_model]
   - `models/stt/vocab.json`                        — token_id → UTF-8 string
   - `models/stt/mel_filters_80.npy`                — whisper's exact mel filterbank [80 × 201]

**Why the .npy files are critical**: the Hailo HEFs use a matmul-split architecture where the
token embedding lookup (decoder input) and output projection (hidden→logits) are offloaded to
the CPU.  Without these files the decoder receives raw float token IDs instead of proper
embeddings, and the output is completely garbage — the decoder will produce timestamp tokens and
other nonsense regardless of what was spoken.

**vocab.json must be generated with `ensure_ascii=False`** (the script does this automatically).
The C++ JSON parser uses `nlohmann::json` which handles both forms, but `ensure_ascii=False`
keeps the file human-readable.  If you regenerate manually, add that flag:
```python
json.dump(..., ensure_ascii=False)
```

### Mel spectrogram — exact whisper compatibility requirements

These details are mandatory for correct Hailo transcription.  Any deviation produces garbage.

**N_FFT = 400, HOP = 160** (not 512).  The whisper ONNX export and the Hailo HEF were
both compiled from whisper's STFT with `n_fft=400`.

**Mixed-radix FFT** — N=400 = 2⁴×5² is not a power of two.  `fft_inplace()` uses
Cooley-Tukey radix-2 recursion down to N=25, then a direct DFT-25 (precomputed twiddle
table in `Dft25Table`).  **Do not replace with a power-of-2 FFT** — that would silently
zero-pad/truncate and produce wrong mel values.

**Hann window of size N_FFT=400** — matches `torch.hann_window(400)`.

**STFT center=True / reflect padding** — PyTorch's default `torch.stft` uses
`center=True`, which reflect-pads `N_FFT/2 = 200` samples at both ends so that frame 0
is centered at sample 0.  `log_mel()` replicates this:
- prepend 200 samples (reflect of first 200 input samples)
- append  200 samples (reflect of last  200 input samples)
Without this, the first few mel frames are systematically wrong.

**O'Shaughnessy mel scale + Slaney normalisation** — whisper uses librosa's default
`htk=False` mel scale (piecewise-linear at low frequency, log above 1 kHz) with
`norm='slaney'` (each filter scaled by `2/bandwidth_hz`).  The HTK formula
`2595*log10(1+f/700)` gives **wrong filter shapes** — peak values ~0.044 vs whisper's
~0.026.  `HailoTranscriber` loads `mel_filters_80.npy` (whisper's exact filterbank) from
the weights directory, bypassing the analytical formula entirely.  The analytical
`hz_to_mel_oshaughnessy()` fallback is only used if the .npy file is missing.

**Mel tensor layout** — `log_mel()` returns a flat vector in row-major
`[mel_band, frame]` order (80 rows × N_FRAMES cols).  The Hailo encoder input is
`FCR(1×1000×80)` = `[frame, mel_band]`.  `run_encoder()` transposes the tensor
before copying to the input buffer.  **Do not skip the transpose** — the encoder will
accept the wrong-shaped buffer without error but produce random outputs.

**Debugging tip**: set `VELAN_DUMP_MEL=/tmp/mel.bin` before running velan.  The binary
has a 2×int32 header `[N_MELS, N_FRAMES]` followed by `N_MELS×N_FRAMES` float32 values
in `[mel_band, frame]` order (pre-transpose).  Compare with Python:
```python
import numpy as np, whisper
model = whisper.load_model("tiny")
mel = whisper.log_mel_spectrogram("test.wav")  # [80, frames] float32
```

### Build for RPi + Hailo-8L
The sysroot is already synced (`~/sdk/rpi/adas`). Build with:
```
./scripts/build_velan.sh --target rpi --aicore hailo8
```
This sets `-DGGML_HAILO8=ON` which:
- Finds `libhailort.so` in `~/sdk/rpi/adas/usr/lib`
- Adds `src/HailoTranscriber.cpp` to the build
- Links against `libhailort` and defines `GGML_USE_HAILO8`

**ALSA env var required on RPi** when using a conan-built PortAudio:
```bash
export ALSA_CONFIG_DIR=/usr/share/alsa
```
Without it, PortAudio fails to open the microphone (ALSA cannot find `alsa.conf`).

### Runtime invocation
```
ALSA_CONFIG_DIR=/usr/share/alsa \
./velan --aicore hailo8 \
        --encoder-hef models/stt/encoder.hef \
        --decoder-hef models/stt/decoder.hef \
        --vocab-json  models/stt/vocab.json
```

### Test mode (offline WAV transcription)
```
ALSA_CONFIG_DIR=/usr/share/alsa \
./velan --aicore hailo8 \
        --encoder-hef ... --decoder-hef ... --vocab-json ... \
        --test-wav /tmp/test.wav
```
Transcribes the WAV and exits — no mic, TTS, LLM, or VHAL required.  Use with
`scripts/test_hailo_wav.sh` to generate and copy a known speech WAV from the host.

### Decoder output format
The tiny HEF decoder emits 4 output tensors with names ending in `_0` through `_3`,
each shaped `FCR(1×32×vpc)` where vpc≈12966 (vocab split).  `decoder_step()` collects
all 4 into one contiguous logits vector `[vocab_size]` and takes argmax for greedy
decoding.  The vocab size must match `token_embedding_weight_tiny.npy` row count.

---

## What NOT to do

- **Do not add `wait_for_silence()` back to WakeWordDetector.** End-of-speech detection belongs
  to STT (record_audio VAD). WWD's responsibility ends at `callback_()`.
- **Do not let WWD and STT run concurrently.** The state machine exists to prevent mic conflicts
  and to stop WWD from picking up TTS audio through the speaker.
- **Do not load a separate Whisper model for WWD.** Share the STT context.
- **Do not call `handle_trigger(0)` to stop recording.** STT stops itself via VAD.
- **Do not use a hold-to-talk pattern** in trigger_velan.py (no auto-repeat timeout).

---

## Models

| Model | Path | Used by |
|-------|------|---------|
| Whisper medium | `models/stt/ggml-medium.bin` | STT + WWD (shared context) |
| Piper voice | `models/tts/en_US-lessac-medium.onnx` | TTS |
| Ollama LLM | `llama3.2:3b` (default) | LLM |

Download: `./scripts/download_models.sh`

---

## Build

All C++ dependencies (gRPC, Protobuf, PortAudio, libcurl, nlohmann/json, whisper.cpp) are
managed by **Conan 2.x**. Install it once:

```bash
pip install conan
conan profile detect   # creates ~/.conan2/profiles/default
```

Then build with:

```bash
./scripts/build_velan.sh --target pc                        # PC, CPU only
./scripts/build_velan.sh --target pc --aicore cuda          # PC, CUDA acceleration
./scripts/build_velan.sh --target rpi                       # Raspberry Pi, CPU only
./scripts/build_velan.sh --target rpi --aicore hailo8       # Raspberry Pi, Hailo-8 accelerator
```

`--target` is required (`pc` or `rpi`). `--aicore` is optional (`cuda` or `hailo8`; default: CPU).

The script runs `conan install` + `source conanbuild.sh` before invoking CMake. The first rpi
build may take 30–60 min (gRPC built from source for aarch64). Subsequent runs use
`~/.conan2` cache.

Cross-compilation additionally requires:
```bash
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

Conan profiles: `profiles/pc` (x86_64) and `profiles/rpi` (armv8, GCC 11).
Local whisper.cpp Conan recipe: `conan/recipes/whisper/conanfile.py`.
