# Velan Architecture

## Role

Velan is a **gRPC client**. It connects to a VHAL core server and subscribes to
vehicle property change events. It does not expose any server port of its own.

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
                                        │ StartPropertyValuesStream
                                        │ (server-streaming RPC)
                                        ▼
                              ┌─────────────────┐
                              │     Velan        │
                              │  (gRPC client)  │
                              └────────┬────────┘
                                       │ filters prop == 0x21400001
                                       │
                    ┌──────────────────┴──────────────────┐
                    │                                     │
              int32_values[0] == 1               int32_values[0] == 0
              (TRIGGER_ON)                       (TRIGGER_OFF)
                    │                                     │
                    ▼                                     ▼
             Start recording                      Stop recording
             (PortAudio)                          (PortAudio)
                                                        │
                                                        ▼
                                               whisper.cpp  (STT)
                                                        │
                                                        ▼
                                               Ollama  (LLM)
                                                        │
                                                        ▼
                                               Piper   (TTS)
                                                        │
                                                        ▼
                                               PortAudio speaker output
```

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

Velan compares each result against the previous value and acts only on changes.
This polling pattern matches what `vhal-gateway` uses and avoids the server-side
write failures that `StartPropertyValuesStream` produces when FakeVehicleHardware
pushes updates from a different thread.

---

## VOICE_ASSIST_TRIGGER Property

| Field | Value |
|-------|-------|
| Property ID | `0x21400001` |
| Group | `VENDOR` (`0x20000000`) |
| Area | `GLOBAL` (`0x01000000`) |
| Type | `INT32` (`0x00400000`) |
| Unique ID | `0x0001` |
| `int32_values[0]` | `1` = ON (start recording), `0` = OFF (stop recording) |

---

## Process Flow

```
Velan start-up
│
├─ Load Whisper model
├─ Connect to VHAL gRPC server (localhost:50051 by default)
└─ Poll loop at 10 Hz  ←── GetValues(VOICE_ASSIST_TRIGGER) every 100 ms
        │
        ├─ server unreachable  →  log once, retry next tick
        │
        ├─ value unchanged     →  skip
        │
        ├─ value changed to 1  →  launch recording thread
        │   (TRIGGER_ON)           (PortAudio, runs until TRIGGER_OFF)
        │
        └─ value changed to 0  →  stop recording thread
            (TRIGGER_OFF)          transcribe (whisper.cpp)
                                   query LLM  (Ollama REST API)
                                   print reply
                                   speak reply (Piper TTS → PortAudio)
```

On SIGINT, SIGTERM, or SIGABRT the poll loop exits within one tick (≤ 100 ms)
and Velan cleans up cleanly. No reconnect delay — the next tick simply retries
if the server is temporarily absent.

---

## Test Tool

`test/trigger_velan.py` is a **gRPC client** test utility — it is not a server.
It sends `SetValues(VOICE_ASSIST_TRIGGER)` to the same VHAL core that Velan
is connected to, which causes VHAL to emit the property change on the stream
that Velan is already listening to.

```
trigger_velan.py  ──SetValues──▶  VHAL core  ──stream──▶  Velan
   (test client)                  (server)               (client)
```

---

## Component Dependencies

| Component | Role | Runs where |
|-----------|------|-----------|
| VHAL core (`vhal-server`) | gRPC server, property bus | Any ECU |
| Velan | gRPC client, voice pipeline | Any ECU |
| `trigger_velan.py` | Test-only trigger client | Development host |
| Ollama | LLM REST server | Same host as Velan |
| whisper.cpp | STT library | Linked into Velan binary |
| Piper | TTS subprocess | Installed on same host |

---

## Source Structure

```
src/
├── main.cpp                      VHAL gRPC polling loop, CLI, signal handling
├── Speech2TextManager.h/.cpp     Singleton: PortAudio capture + Whisper STT
├── TransformerManager.h/.cpp     Ollama conversation history + HTTP chat
└── Text2SpeechManager.h/.cpp     Piper TTS subprocess + PortAudio playback

scripts/
├── build.sh                      Configure + parallel build (Ninja, --cuda flag)
└── download_models.sh            Download Whisper and Piper voice models

models/
├── stt/
│   └── ggml-medium.bin           Whisper medium model (downloaded by script)
└── tts/
    ├── en_US-lessac-medium.onnx       Piper voice model (downloaded by script)
    └── en_US-lessac-medium.onnx.json  Piper voice config (downloaded by script)

~/.local/                         User-local install (outside the project tree)
├── bin/piper                     Wrapper script — sets LD_LIBRARY_PATH, execs real binary
└── lib/piper/                    Piper real binary + bundled shared libs
```

### Class responsibilities

**`Speech2TextManager`** (singleton) owns the speech-input pipeline:
- Constructor initialises PortAudio and loads the Whisper model; throws on failure
- Destructor joins any active recording thread, frees Whisper context, terminates PortAudio
- Spawns and tears down the recording thread on VHAL trigger events (`handle_trigger`)
- Transcribes captured PCM via whisper.cpp, queries the LLM, and speaks the reply (`process`, private)
- Owns `TransformerManager` and `Text2SpeechManager` by value

**`TransformerManager`** owns the LLM interaction:
- Holds the Ollama model name and the full multi-turn conversation `history`
- Seeds the conversation with the Velan system prompt on construction
- Sends each transcript to the Ollama REST API and appends both user and assistant turns to history (`chat`)

**`Text2SpeechManager`** owns the speech-output pipeline:
- Constructor verifies the Piper voice model file is present; throws with download instructions if not
- `speak()` forks a `piper` subprocess, writes text to its stdin, reads back raw 16-bit PCM from stdout, and plays it via PortAudio at 22050 Hz

### Naming rationale

`Speech2TextManager` and `Text2SpeechManager` name the direction of conversion explicitly, making the pipeline readable left-to-right. `TransformerManager` reflects that the class drives a transformer-based LLM, keeping HTTP and history logic separate from audio concerns.

### Composition and call chain

`Speech2TextManager` owns `TransformerManager` and `Text2SpeechManager` by value. The full pipeline on TRIGGER_OFF:

```
Speech2TextManager::handle_trigger(0)
  └─ Speech2TextManager::process()
       ├─ transcribe()               (whisper.cpp, file-local)
       ├─ TransformerManager::chat()
       │    └─ Ollama REST API       (libcurl, file-local)
       └─ Text2SpeechManager::speak()
            ├─ fork + exec piper
            └─ PortAudio output stream
```
