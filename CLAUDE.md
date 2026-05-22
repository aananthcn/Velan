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
- `CHUNK_MS = 100` — mic poll interval
- `END_SILENCE_CHUNKS = 5` — 500 ms of trailing silence ends a speech utterance
- `MIN_PHRASE_FRAMES` — minimum 200 ms of speech before transcribing
- `MAX_BUFFER_MS = 5000` — hard cap on accumulated speech buffer

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
