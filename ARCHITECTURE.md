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
                                                  Terminal output
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
