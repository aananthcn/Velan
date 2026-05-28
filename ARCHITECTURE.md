# Velan Architecture

## Role

Velan is a **gRPC client** (connects to VHAL core) and simultaneously a
**gRPC server** (streams assistant state to any connected UI client on port 50052).

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
                              ┌──────────────────────┐
                              │        Velan          │
                              │  gRPC client :50051   │
                              │  gRPC server :50052   │◄─── velan-ui (any host)
                              └──────────┬────────────┘
                                         │
             ┌───────────────────────────┴─────────────────────┐
             │  VHAL gRPC trigger                               │  Wake word
             │  prop == 0x21400001                              │  (ITranscriber)
             │                                                  │
             │  TRIGGER_ON  (1) ───────────────────────────────►│
             │  TRIGGER_OFF (0) ── ignored (VAD stops STT)      │
             │                                                  │
             └───────────────────┬──────────────────────────────┘
                                 │  both call
                                 ▼
                   Speech2TextManager::handle_trigger()
                   UIServer::notify(RECORDING)
                                 │
                                 ▼
                   LISTENING → PROCESSING transition
                   wwd->pause()  (WWD blocks on cv)
                                 │
                                 ▼
                   record_audio()   ← VAD self-terminates
                   transcribe()     ← ITranscriber (whisper.cpp or Hailo-8L NPU)
                   UIServer::notify(THINKING, transcript)
                   chat()           ← Ollama REST API
                   UIServer::notify(TALKING, transcript, response)
                   speak()          ← Piper TTS → PortAudio
                                 │
                                 ▼
                   PROCESSING → LISTENING transition
                   wwd->resume()  (WWD unblocks)
                   UIServer::notify(LISTENING)
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

## gRPC Contracts

### VHAL (client side)

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

### VelanUIService (server side)

Defined in `proto/velan_ui.proto`. Velan starts a gRPC server on `--ui-port`
(default 50052) and implements one RPC:

| RPC | Direction | Purpose |
|-----|-----------|---------|
| `WatchState` | Server-streaming | Push `StateUpdate` on every pipeline state change |

```
enum AssistantState { IDLE=0, LISTENING=1, RECORDING=2, THINKING=3, TALKING=4 }

message StateUpdate {
    AssistantState state      = 1;
    int64  timestamp_ms       = 2;
    string transcript         = 3;   // set when → THINKING
    string response           = 4;   // set when → TALKING
}
```

`UIServer` (implemented in `src/UIServer.cpp`) runs on a background thread.
`notify()` is called from the voice pipeline and broadcasts to all connected
clients. If no client is connected the call is a no-op.

`velan-ui` (Qt Quick app in `src/ui/`) is the reference UI client. It can run
on any networked host — connect with:
```bash
velan-ui --server <velan-host>:50052
```

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

## Transcriber Abstraction (`ITranscriber`)

All speech-to-text inference goes through the `ITranscriber` interface
(`src/Transcriber.h`):

```cpp
class ITranscriber {
public:
    virtual std::string transcribe(const std::vector<float>& pcm,
                                   bool single_segment) = 0;
    virtual ~ITranscriber() = default;
};
```

Two concrete implementations:

| Class | File | Backend | When used |
|-------|------|---------|-----------|
| `WhisperTranscriber` | `src/WhisperTranscriber.cpp` | whisper.cpp (CPU/CUDA) | `--aicore whisper` (default on PC) |
| `HailoTranscriber` | `src/HailoTranscriber.cpp` | HailoRT NPU (Hailo-8L) | `--aicore hailo8` (default on RPi) |

`g_transcriber` is a `unique_ptr<ITranscriber>` created in `main()` before any
other component. Both `WakeWordDetector` and `Speech2TextManager` receive a raw
`ITranscriber*` pointer to the same instance.

---

## Shared Transcriber Context

`WakeWordDetector` and `Speech2TextManager` share a **single transcriber
instance** created in `main()`.

**Why sharing is safe:** LISTENING and PROCESSING are mutually exclusive — the
transcriber is never accessed concurrently.

**WhisperTranscriber path:** the tiny model (`ggml-tiny.bin`) was too inaccurate
for "Subramani" — hallucinated "Hey Bella", "Hey where are you?" etc. The medium
model (`ggml-medium.bin`) is used for both STT and WWD. `--sttmodel` controls
the path for both.

**HailoTranscriber path:** the NPU HEF files are fixed at compile/deploy time
(`encoder.hef` + `decoder.hef`). No whisper.cpp context is involved. WWD and
STT share the same `HailoTranscriber` instance; the `VDevice` is opened once and
held for the process lifetime. Memory cost: one VDevice + HEF load, not two.

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
├─ g_transcriber = make_unique<HailoTranscriber | WhisperTranscriber>(...)
│                              ← one transcriber for the whole process
│
├─ [--test-wav mode] read WAV → g_transcriber->transcribe() → print → exit
│
├─ g_llm = TransformerManager(ollama_model, ollama_host)
├─ g_tts = Text2SpeechManager(tts_model)
│
├─ Speech2TextManager::instance(g_transcriber.get(), on_start, on_done, mic)
│                              ← owns PROCESSING pipeline; receives ITranscriber*
├─ g_wwd = WakeWordDetector(callback, g_transcriber.get(), phrases, mic)
├─ g_wwd->start()              ← LISTENING begins
│
├─ WakeWordDetector detect_loop (parallel thread)
│    └─ onset-triggered 1000 ms capture → ITranscriber::transcribe()
│         → phrase match → closes mic → callback_() → blocks on cv (PROCESSING)
│
└─ gRPC poll loop at 10 Hz
       └─ VOICE_ASSIST_TRIGGER changed to 1
                          │
                          ▼
          Speech2TextManager::handle_trigger()   (idempotent via is_recording_)
                          │
                          ├─ on_start_()  →  g_wwd->pause()
                          │
                          └─ rec_thread_:
                               record_audio()     ← PortAudio, VAD self-terminates
                               process()
                                 ├─ ITranscriber::transcribe()
                                 ├─ g_llm->chat()
                                 │    └─ Ollama REST API    (libcurl → host PC)
                                 └─ g_tts->speak()
                                      ├─ fork + exec piper
                                      └─ PortAudio output stream
                               on_done_()  →  g_wwd->resume()   (LISTENING resumes)
```

---

## VAD Parameters

Both components use energy-based Voice Activity Detection (VAD): they compute the
RMS (root-mean-square) of each audio chunk and compare it against a threshold to
decide whether speech is present.

### WakeWordDetector (`src/WakeWordDetector.cpp`)

WWD uses an **onset-triggered fixed-duration capture** rather than VAD-based end
detection. Once any chunk crosses the energy threshold, it captures a fixed
`LOCK_IN_MS` window regardless of per-chunk energy. This prevents mid-word
phonemes from falling below threshold and producing mostly-silent buffers.

| Parameter | Value | Meaning |
|-----------|-------|---------|
| `CHUNK_MS` | 100 ms | mic poll interval; one VAD decision per chunk |
| `LOCK_IN_MS` | 1000 ms | fixed capture window after onset — covers longest wake phrase ("Subrahmanya" ≈ 700 ms) |
| `MIN_PHRASE_MS` | 200 ms | safety floor; always satisfied with 1000 ms capture |
| `WWD_NOISE_CAL_CHUNKS` | 5 | ~500 ms calibration at startup |
| `WWD_NOISE_MULTIPLIER` | 1.2× | `eff_threshold = median(cal_rms) × 1.2`; capped at 0.10 |

**Median calibration**: calibration RMS values are sorted and the median is used
as the noise floor. This rejects startup transient spikes (keyboard click, AC
noise) that would inflate a mean-based estimate and set the threshold too high.

### Speech2TextManager (`src/Speech2TextManager.cpp`)

STT uses an **adaptive threshold** because it must reliably detect *end of
speech* in environments with varying background noise. A fixed threshold that
works in a quiet room will fail in a car cabin or open-plan office.

#### Calibration phase

At the start of each recording, the first `VAD_NOISE_CAL_CHUNKS` audio chunks
are used to measure the ambient noise level:

```
noise_floor  = average RMS over the calibration window
vad_threshold = clamp(noise_floor × VAD_NOISE_MULTIPLIER,
                      VAD_RMS_THRESHOLD,       ← floor
                      VAD_THRESHOLD_MAX)        ← ceiling
```

The result is logged as:
```
[Velan] Noise floor: 0.031  VAD threshold: 0.093
```

#### Parameters

| Parameter | Value | Meaning |
|-----------|-------|---------|
| `VAD_NOISE_CAL_CHUNKS` | 5 | ~320 ms of audio used to measure ambient noise at the start of each recording |
| `VAD_RMS_THRESHOLD` | 0.02 | minimum threshold floor — ensures the adaptive value never drops so low that background hiss triggers speech |
| `VAD_NOISE_MULTIPLIER` | 3.0 | speech must be 3× louder than the ambient noise floor to register as speech |
| `VAD_THRESHOLD_MAX` | 0.10 | ceiling — prevents the threshold from rising so high that normal speech (typically 0.05–0.20 RMS) is never detected in loud rooms |
| `VAD_MIN_SPEECH_CHUNKS` | 5 | ~320 ms of audio above threshold required before the silence counter starts; avoids reacting to a single noise spike |
| `VAD_SILENCE_CHUNKS` | 20 | 20 chunks × 64 ms ≈ 1.3 s of continuous silence after speech ends the recording; the longer window tolerates occasional noise spikes in the post-speech period |
| `VAD_NO_SPEECH_TIMEOUT` | 125 | ~8 s limit: if speech never starts after calibration (e.g. accidental wake-word trigger), recording is aborted instead of running to the hard cap |
| `VAD_MAX_RECORD_FRAMES` | 30 s | absolute hard cap; guards against VAD failure in extreme noise conditions |

#### Why the ceiling matters (VAD_THRESHOLD_MAX)

Without a ceiling, a loud background (noise floor ≈ 0.088) would set the threshold
to `0.088 × 3 = 0.264`. Normal conversational speech typically reaches 0.10–0.20
RMS — below 0.264 — so the VAD would never see any speech, `speech_started`
would never be set, and recording would run all the way to the 30 s hard cap.

The ceiling at 0.10 ensures that in any environment where it is physically
possible to hear the wake word, it is also possible to detect the subsequent
speech.

#### Recording termination logic

```
After calibration, for each chunk:

  rms ≥ vad_threshold?
  ├── YES → speech_chunks++; silence_chunks = 0
  │         speech_started = (speech_chunks ≥ VAD_MIN_SPEECH_CHUNKS)
  │
  └── NO, speech_started = true  → silence_chunks++
  │         silence_chunks ≥ VAD_SILENCE_CHUNKS? → END OF SPEECH ✓
  │
  └── NO, speech_started = false → silence_chunks++
            silence_chunks ≥ VAD_NO_SPEECH_TIMEOUT? → NO SPEECH DETECTED ✓
```

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
| Velan | gRPC client (VHAL) + gRPC server (UI :50052), voice pipeline | Any ECU |
| `velan-ui` | Qt Quick Siri-orb UI, gRPC client to Velan :50052 | Any networked host |
| `trigger_velan.py` | Test-only trigger client | Development host |
| Ollama | LLM REST server | Host PC (RPi deployment) or same host (PC deployment) |
| whisper.cpp / `WhisperTranscriber` | STT + WWD on CPU/CUDA | Linked into Velan binary |
| HailoRT / `HailoTranscriber` | STT + WWD on Hailo-8L NPU | Linked into Velan binary (RPi only) |
| Piper | TTS subprocess | Installed on same host as Velan |

**Ollama location when deploying to RPi:** vhal-core and Ollama both run on the
host PC. Velan on the RPi connects to Ollama over the LAN. `run_velan.sh`
auto-detects the PC's LAN IP and passes `--llm <host_ip>` to velan.  Ollama
must be configured to bind to all interfaces:
```bash
OLLAMA_HOST=0.0.0.0 ollama serve
# or: Environment="OLLAMA_HOST=0.0.0.0" in the systemd service
```

---

## Models

### whisper.cpp mode (CPU/CUDA)

| Model | Path | Used by |
|-------|------|---------|
| Whisper medium | `models/stt/ggml-medium.bin` | STT + WWD (shared context) |
| Piper voice | `models/tts/en_US-lessac-medium.onnx` | TTS |
| Ollama LLM | `llama3.2:3b` (default) | LLM |

Download: `./scripts/download_models.sh`

### Hailo-8L NPU mode

| File | Path | Used by |
|------|------|---------|
| `encoder.hef` | `models/stt/whisper-tiny-h8l/` | NPU encoder |
| `decoder.hef` | `models/stt/whisper-tiny-h8l/` | NPU decoder |
| `token_embedding_weight_tiny.npy` | `models/stt/whisper-tiny-h8l/weights/` | CPU embedding lookup |
| `onnx_add_input_tiny.npy` | `models/stt/whisper-tiny-h8l/weights/` | CPU positional encoding |
| `vocab.json` | `models/stt/whisper-tiny-h8l/weights/` | Detokeniser |
| `mel_filters_80.npy` | `models/stt/whisper-tiny-h8l/weights/` | Exact whisper mel filterbank |

Generate the weight/vocab files once (any machine with Python):
```bash
pip install openai-whisper
python3 scripts/extract_whisper_weights.py --model tiny \
        --out models/stt/whisper-tiny-h8l/weights
```

HEF files are obtained from Hailo's Model Zoo (not in this repo).

---

## Source Structure

```
proto/
└── velan_ui.proto                VelanUIService definition (AssistantState stream)

src/
├── main.cpp                      CLI, VHAL gRPC polling loop, signal handling, init order
├── UIServer.h/.cpp               gRPC server: streams AssistantState to velan-ui clients
├── Transcriber.h                 ITranscriber interface (transcribe() pure virtual)
├── WhisperTranscriber.h/.cpp     ITranscriber via whisper.cpp (CPU/CUDA)
├── HailoTranscriber.h/.cpp       ITranscriber via HailoRT NPU (Hailo-8L)
├── Speech2TextManager.h/.cpp     Singleton: record → ITranscriber → on_transcript pipeline
├── TransformerManager.h/.cpp     Ollama multi-turn conversation (model + history)
├── Text2SpeechManager.h/.cpp     Piper TTS subprocess + PortAudio playback
├── WakeWordDetector.h/.cpp       Onset-triggered capture → ITranscriber → phrase match
└── ui/                           Qt Quick Siri-orb UI (built if Qt6 found)
    ├── main.cpp                  QGuiApplication + QQmlEngine, --server CLI arg
    ├── GrpcStateWatcher.h/.cpp   QThread: streams WatchState RPC, emits stateChanged()
    ├── main.qml                  Window, state wiring, status overlay, transcript label
    ├── SiriOrb.qml               Animated orb component (5 states, breathing/swirl/colour)
    └── qml.qrc                   Qt resource bundle

conan/
├── recipes/whisper/conanfile.py  Local Conan recipe: builds whisper.cpp v1.7.4
└── recipes/portaudio/conanfile.py Local Conan recipe: PortAudio with ALSA

profiles/
├── pc                            Conan profile: x86_64 native (build == host)
└── rpi                           Conan profile: armv8 cross-compile (GCC 11, aarch64-linux-gnu)

conanfile.py                      Root Conan file: all deps + grpc tool_requires

scripts/
├── build_velan.sh                conan install + cmake (pc|rpi, cpu|cuda|hailo8)
├── deploy_velan.sh               Deploy binary + Piper + models to /opt/car-ui/
├── run_velan.sh                  Start vhal-core (local) + velan (local or RPi via SSH)
├── do_all.sh                     build → deploy → run in one command
├── download_models.sh            Download Whisper medium + Piper binary
├── download_hailo8_models.sh     Download/check Hailo HEF files
├── extract_whisper_weights.py    One-time: extract NPU weights + mel filterbank + vocab.json
├── test_hailo_wav.sh             Generate TTS WAV + SCP to RPi + run --test-wav
└── sync_rpi_sysroot.sh           Sync RPi sysroot to ~/sdk/rpi/adas

models/
├── stt/ggml-medium.bin                            Whisper medium (CPU/CUDA)
├── stt/whisper-tiny-h8l/encoder.hef               Hailo NPU encoder
├── stt/whisper-tiny-h8l/decoder.hef               Hailo NPU decoder
└── stt/whisper-tiny-h8l/weights/
    ├── token_embedding_weight_tiny.npy             CPU embedding lookup
    ├── onnx_add_input_tiny.npy                     CPU positional encoding
    ├── mel_filters_80.npy                          Whisper's exact mel filterbank
    └── vocab.json                                  Token-ID → UTF-8 string
```

### Class responsibilities

**`HailoTranscriber`** (new) implements `ITranscriber` for NPU inference:
- Loads `encoder.hef` + `decoder.hef` via HailoRT `VDevice` + `ConfiguredInferModel`
- `transcribe(pcm)`: `log_mel()` (CPU, N_FFT=400 mixed-radix, center=True, exact
  whisper filterbank) → `run_encoder()` (NPU, transpose [mel×frames]→[frames×mel])
  → `greedy_decode()` (NPU, autoregressive with EOT / repeat / n-gram cycle guards)
  → `detokenise()` (CPU, vocab.json lookup)
- All weight .npy files loaded at startup; no hot-path I/O
- `VDevice` is opened once and held for the process lifetime

**`WhisperTranscriber`** wraps whisper.cpp as `ITranscriber`:
- Loads ggml model file; exposes `whisper_context*` for sharing
- `transcribe()` calls `whisper_full()` and extracts text segments

**`Speech2TextManager`** (singleton) owns the PROCESSING pipeline:
- Receives `ITranscriber*` and two callbacks (`on_start`, `on_transcript`) on construction
- `handle_trigger()`: idempotent entry point (guarded by `is_recording_.exchange(true)`)
- Spawns `rec_thread_` → `record_audio()` (VAD self-terminates) → `transcribe()` → `on_transcript()`
- `on_transcript` callback (in main.cpp) calls `g_llm->chat()` → `g_tts->speak()` → `g_wwd->resume()`

**`WakeWordDetector`** owns the LISTENING pipeline:
- Receives `ITranscriber*`; does **not** own it
- `detect_loop()`: onset-triggered 1000 ms capture → `ITranscriber::transcribe()` → phrase match
- On match: closes mic, calls `callback_()`, blocks on `condition_variable` until `resume()`

**`TransformerManager`** owns the LLM interaction:
- `chat()` POSTs JSON to `http://<ollama_host>:11434/api/chat` via libcurl
- Maintains multi-turn history across the session

**`Text2SpeechManager`** owns TTS output:
- `speak()` forks `piper`, writes text to stdin, reads 16-bit PCM from stdout, plays via PortAudio

### Initialisation order in main()

```
1. g_transcriber = make_unique<HailoTranscriber | WhisperTranscriber>(...)
                              ← one shared transcriber instance
2. [--test-wav] → transcribe WAV → print → return 0   (no TTS/LLM/mic/VHAL needed)
3. g_llm = TransformerManager(model, ollama_host)
4. g_tts = Text2SpeechManager(tts_model)
5. Speech2TextManager::instance(g_transcriber.get(), on_start, on_transcript, mic)
6. g_wwd = WakeWordDetector(callback, g_transcriber.get(), phrases, mic)
7. g_wwd->start()             ← LISTENING begins
8. gRPC poll loop
```

Steps 3–4 are after the `--test-wav` early-exit so TTS/LLM init is not required
for offline testing. The `on_transcript` lambda in step 5 captures `g_wwd` by
reference; the pointer is null-safe until step 6.

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

`velan-ui` is built automatically when Qt6 is detected. To install Qt6:
```bash
sudo apt install qt6-base-dev qt6-declarative-dev
```
To skip the UI build: `cmake -DBUILD_UI=OFF …`

Prerequisites:
- `pip install conan && conan profile detect` (once per machine)
- `sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu` (rpi target only)

### Running with UI

```bash
# PC (UI on same machine):
./scripts/run_velan.sh --target pc

# RPi (UI on PC, Velan on RPi):
./scripts/run_velan.sh --target rpi --llm 192.168.10.1

# RPi (UI on a different machine, e.g. laptop at 192.168.10.50):
./scripts/run_velan.sh --target rpi --llm 192.168.10.1 --no-ui
# On the laptop:
velan-ui --server 192.168.10.30:50052

# Disable UI server entirely:
./scripts/run_velan.sh --target rpi --ui-port 0
```

First rpi build: Conan builds gRPC + abseil from source for aarch64 (~30–60 min).
Subsequent builds use `~/.conan2` binary cache.
