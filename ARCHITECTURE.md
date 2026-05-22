# Velan Architecture

## Role

Velan is a **gRPC client**. It connects to a VHAL core server and polls for a
vendor-defined trigger property. It does not expose any server port of its own.

---

## System Overview

```
┌─────────────────────────────────────────────────────┐
│  ECU  (IVI / Cluster / ADAS)                        │
│                                                     │
│  ┌──────────────┐   SetValues(VOICE_ASSIST_TRIGGER) │
│  │  HMI / Button│──────────────────────┐            │
│  └──────────────┘                      ▼            │
│                              ┌─────────────────┐    │
│                              │   VHAL core     │    │
│                              │  (gRPC server)  │    │
│                              │  0.0.0.0:50051  │    │
│                              └────────┬────────┘    │
└───────────────────────────────────────┼─────────────┘
                                        │ GetValues (10 Hz poll)
                                        ▼
                              ┌─────────────────┐
                              │     Velan        │
                              │  (gRPC client)  │
                              └────────┬────────┘
                                       │
             ┌─────────────────────────┴──────────────────────┐
             │  VHAL gRPC trigger                              │  Wake word
             │  prop == 0x21400001                             │  (whisper.cpp)
             │                                                 │
             │  TRIGGER_ON  (1) ──────────────────────────────►│
             │  TRIGGER_OFF (0) ── ignored (VAD stops STT)     │
             │                                                 │
             └──────────────────┬──────────────────────────────┘
                                │  both call
                                ▼
                  Speech2TextManager::handle_trigger()
                                │
                                ▼
                  LISTENING → PROCESSING transition
                  wwd->pause()  (WWD blocks on cv)
                                │
                                ▼
                  record_audio()   ← VAD self-terminates
                  transcribe()     ← whisper.cpp
                  chat()           ← Ollama REST API
                  speak()          ← Piper TTS → PortAudio
                                │
                                ▼
                  PROCESSING → LISTENING transition
                  wwd->resume()  (WWD unblocks)
```

---

## Pipeline States

The system is always in exactly one of two mutually exclusive states:

| State | Mic owner | Other component |
|-------|-----------|-----------------|
| `LISTENING` | `WakeWordDetector` | `Speech2TextManager` is idle |
| `PROCESSING` | `Speech2TextManager` | `WakeWordDetector` is blocked on a condition variable |

ALSA in exclusive mode allows only one stream owner at a time. The state machine
enforces this at the software level.

### State transitions

| Event | From | To | Mechanism |
|-------|------|----|-----------|
| Wake word match | LISTENING | PROCESSING | `callback_()` → `handle_trigger()` → `wwd->pause()` |
| VHAL TRIGGER_ON | LISTENING | PROCESSING | gRPC poll → `handle_trigger()` → `wwd->pause()` |
| VAD silence / timeout | PROCESSING | LISTENING | end of `rec_thread_` → `on_done_()` → `wwd->resume()` |

VHAL TRIGGER_OFF (state == 0) is **ignored**. STT manages its own end-of-speech
via VAD silence detection.

---

## gRPC Contract

Velan uses the `VehicleServer` service defined in vhal-core:

```
~/labs/networking/vhal-core/test/vhal/VehicleServer.proto
```

One RPC is called repeatedly at 10 Hz:

| RPC | Direction | Purpose |
|-----|-----------|---------|
| `GetValues` | Unary | Read current value of `VOICE_ASSIST_TRIGGER` |

Velan compares each result against the previous value and acts only on a rising
edge (0 → 1). This polling pattern matches what `vhal-gateway` uses and avoids
the server-side write failures that `StartPropertyValuesStream` produces when
FakeVehicleHardware pushes updates from a different thread.

---

## VOICE_ASSIST_TRIGGER Property

| Field | Value |
|-------|-------|
| Property ID | `0x21400001` |
| Group | `VENDOR` (`0x20000000`) |
| Area | `GLOBAL` (`0x01000000`) |
| Type | `INT32` (`0x00400000`) |
| Unique ID | `0x0001` |
| `int32_values[0]` | `1` = start PROCESSING, `0` = ignored |

---

## Shared Whisper Context

`WakeWordDetector` and `Speech2TextManager` share a single `whisper_context*`
loaded by `Speech2TextManager` at startup.

The tiny model (`ggml-tiny.bin`) was too inaccurate for "Subramani" — it
hallucinated "Hey Bella", "Hey where are you?" etc. The medium model
(`ggml-medium.bin`) is used for both STT and wake-word detection.

Sharing is safe because LISTENING and PROCESSING are mutually exclusive — the
context is never accessed concurrently. Memory cost: one model (~1.5 GB) instead
of two (~1.6 GB for tiny + medium).

`--sttmodel` controls the model path for both STT and WWD.

---

## Wake Phrases

`--wwphrase` accepts a comma-separated list. Any match fires the trigger.

```
./velan --wwphrase "Hey Vela, Subramanya, Subramani, Subrahmanya"
```

Default: `"Hey Vela, Subramanya, Subramani, Subrahmanya"`

Each phrase is normalised (lower-case, punctuation → spaces, runs collapsed) and
stored in a vector. `phrase_matches()` checks substring match against all of them.

---

## Process Flow

```
Velan start-up
│
├─ Speech2TextManager::instance()   ← loads Whisper model, owns on_start + on_done
├─ WakeWordDetector(ctx)            ← receives shared whisper_context*
├─ wwd->start()                     ← LISTENING begins
│
├─ WakeWordDetector detect_loop (parallel thread)
│    └─ VAD-gated sliding window → whisper_full() → phrase match
│         └─ match → closes mic → callback_() → blocks on cv (PROCESSING)
│
└─ gRPC poll loop at 10 Hz
       └─ VOICE_ASSIST_TRIGGER changed to 1
                          │
                          ▼
          Speech2TextManager::handle_trigger()   (idempotent via is_recording_)
                          │
                          ├─ on_start_()  →  wwd->pause()
                          │
                          └─ rec_thread_:
                               record_audio()     ← PortAudio, VAD self-terminates
                               process()
                                 ├─ transcribe()            (whisper.cpp)
                                 ├─ TransformerManager::chat()
                                 │    └─ Ollama REST API    (libcurl)
                                 └─ Text2SpeechManager::speak()
                                      ├─ fork + exec piper
                                      └─ PortAudio output stream
                               on_done_()  →  wwd->resume()   (LISTENING resumes)
```

---

## VAD Parameters

### WakeWordDetector
| Parameter | Value | Meaning |
|-----------|-------|---------|
| `CHUNK_MS` | 100 ms | mic poll interval |
| `END_SILENCE_CHUNKS` | 5 | 500 ms trailing silence ends utterance |
| `MIN_PHRASE_FRAMES` | — | 200 ms minimum speech before transcribing |
| `MAX_BUFFER_MS` | 5000 ms | hard cap on accumulated speech buffer |

### Speech2TextManager
| Parameter | Value | Meaning |
|-----------|-------|---------|
| `VAD_RMS_THRESHOLD` | 0.01 | energy floor for speech detection |
| `VAD_MIN_SPEECH_CHUNKS` | 5 | ~320 ms speech required before silence counting |
| `VAD_SILENCE_CHUNKS` | 15 | ~960 ms sustained silence ends recording |
| `VAD_MAX_RECORD_FRAMES` | 90 s | hard cap; guards against VAD failure in noisy environments |

---

## Test Tool

`test/trigger_velan.py` is a **gRPC client** test utility — it is not a server.
It sends `SetValues(VOICE_ASSIST_TRIGGER)` to the VHAL core that Velan is
connected to.

```
trigger_velan.py  ──SetValues──▶  VHAL core  ──GetValues──▶  Velan
   (test client)                  (server)                   (client)
```

| Key | Action |
|-----|--------|
| ENTER | `TRIGGER_ON` (state = 1) — starts PROCESSING |
| BACKSPACE or ESC | `TRIGGER_OFF` (state = 0) — sent for completeness; Velan ignores it |
| Ctrl-C | quit |

This is **press-to-start**, not hold-to-talk. STT self-terminates via VAD.

---

## Component Dependencies

| Component | Role | Runs where |
|-----------|------|-----------|
| VHAL core (`vhal-server`) | gRPC server, property bus | Any ECU |
| Velan | gRPC client, voice pipeline | Any ECU |
| `trigger_velan.py` | Test-only trigger client | Development host |
| Ollama | LLM REST server | Same host as Velan |
| whisper.cpp | STT + WWD library (shared context) | Linked into Velan binary |
| Piper | TTS subprocess | Installed on same host |

---

## Models

| Model | Path | Used by |
|-------|------|---------|
| Whisper medium | `models/stt/ggml-medium.bin` | STT + WWD (shared context) |
| Piper voice | `models/tts/en_US-lessac-medium.onnx` | TTS |
| Ollama LLM | `llama3.2:3b` (default) | LLM |

Download: `./scripts/download_models.sh`

---

## Source Structure

```
src/
├── main.cpp                      CLI, VHAL gRPC polling loop, signal handling, init order
├── Speech2TextManager.h/.cpp     Singleton: record → STT → LLM → TTS pipeline
├── TransformerManager.h/.cpp     Ollama multi-turn conversation (model + history)
├── Text2SpeechManager.h/.cpp     Piper TTS subprocess + PortAudio playback
└── WakeWordDetector.h/.cpp       Whisper sliding-window detector, pause/resume state machine

conan/
└── recipes/whisper/conanfile.py  Local Conan recipe: builds whisper.cpp v1.7.4 as a package

profiles/
├── pc                            Conan profile: x86_64 native (build == host)
└── rpi                           Conan profile: armv8 cross-compile (GCC 11, aarch64-linux-gnu)

conanfile.py                      Root Conan file: all deps + grpc tool_requires

scripts/
├── build_velan.sh                conan create + conan install + cmake (pc|rpi, cuda|hailo8)
├── run_velan.sh                  Start vhal-core + velan together; Ctrl-C stops both
├── deploy_velan.sh               Deploy built binary to RPi over SSH
└── download_models.sh            Download Whisper medium + Piper models

models/
├── stt/ggml-medium.bin           Whisper medium (shared by STT + WWD)
└── tts/en_US-lessac-medium.onnx  Piper voice model
```

### Class responsibilities

**`Speech2TextManager`** (singleton) owns the PROCESSING pipeline:
- Loads the Whisper model and initialises PortAudio on construction; throws on failure
- `handle_trigger()`: entry point for both VHAL and wake-word triggers; idempotent via `is_recording_`
- Spawns `rec_thread_` which runs `record_audio()` → `process()` → `on_done_()`
- `record_audio()` uses internal VAD (silence detection) to stop itself
- Owns `TransformerManager` and `Text2SpeechManager` by value
- Exposes `get_context()` so `WakeWordDetector` can share the loaded `whisper_context*`

**`WakeWordDetector`** owns the LISTENING pipeline:
- Receives the shared `whisper_context*` from `Speech2TextManager` on construction
- `start()` spawns `detect_loop`; `stop()` sets the stop flag and joins
- `detect_loop()` runs a VAD-gated sliding window over mic input; calls `whisper_full()` on each speech utterance; fires `callback_()` on phrase match
- On phrase match: closes its mic stream, calls `callback_()`, then **blocks on a condition variable** until `resume()` is called
- `pause()` / `resume()`: called by `Speech2TextManager` at LISTENING↔PROCESSING transitions

**`TransformerManager`** owns the LLM interaction:
- Holds the Ollama model name and the full multi-turn conversation history
- Seeds the conversation with the Velan system prompt on construction
- `chat()` sends each transcript to the Ollama REST API and appends both turns to history

**`Text2SpeechManager`** owns the speech-output pipeline:
- `speak()` forks a `piper` subprocess, writes text to its stdin, reads raw 16-bit PCM from stdout, plays it via PortAudio at 22050 Hz

### Initialisation order in main()

```
1. Declare wwd (unique_ptr, null)           — lambdas capture by ref; safe before step 4
2. Speech2TextManager::instance()           — loads Whisper model, stores on_start + on_done
3. WakeWordDetector(mgr.get_context(), ...) — receives shared context
4. wwd->start()                             — LISTENING begins
```

This order is required: WWD needs the `whisper_context*`, which only exists after step 2.

### Dependency management

All C++ dependencies are managed by **Conan 2.x** — no system-level
gRPC/Protobuf/PortAudio/libcurl packages needed.

| Package | Conan name | Who uses it |
|---------|-----------|------------|
| gRPC + Protobuf | `grpc/1.54.3` | VHAL gRPC client + proto codegen |
| PortAudio | `portaudio/19.7` | Mic capture + speaker playback |
| libcurl | `libcurl/8.6.0` | Ollama REST API |
| nlohmann/json | `nlohmann_json/3.11.3` | JSON parsing |
| whisper.cpp | local recipe `conan/recipes/whisper/` | STT + WWD |

`grpc/1.54.3` is declared as both a `requires` (library for the target) and a
`tool_requires` (protoc + grpc_cpp_plugin for the build machine), guaranteeing
version consistency and eliminating the class of tool/library mismatch errors
that plagued the old sysroot approach.

Hailo8 SDK is **not** Conan-managed (proprietary). Install it manually on the
target; `GGML_HAILO8=ON` in CMake will find it from the system path.

Conan profiles: `profiles/pc` (x86_64), `profiles/rpi` (armv8 / aarch64).

### Build targets

```bash
./scripts/build_velan.sh --target pc                        # PC, CPU only
./scripts/build_velan.sh --target pc --aicore cuda          # PC, CUDA acceleration
./scripts/build_velan.sh --target rpi                       # Raspberry Pi, CPU only
./scripts/build_velan.sh --target rpi --aicore hailo8       # Raspberry Pi, Hailo-8 NPU
```

Prerequisites:
- `pip install conan && conan profile detect` (once per machine)
- `sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu` (rpi target only)

First rpi build: Conan builds gRPC + abseil from source for aarch64 (~30–60 min).
Subsequent builds use `~/.conan2` binary cache.
