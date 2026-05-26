# Velan (C++), the AI Assistant for Automotive

A local automotive voice assistant that listens to voice commands, transcribes
them with [whisper.cpp](https://github.com/ggerganov/whisper.cpp), sends the
text to a local LLM via [Ollama](https://ollama.com), and speaks the reply
aloud using [Piper](https://github.com/rhasspy/piper) TTS. No cloud APIs, no
Python runtime required at execution time.

Developed on a **PC with Nvidia GPU** (RTX 5070), then deployed to
**Raspberry Pi 5** with the
[Raspberry Pi AI HAT+](https://www.raspberrypi.com/products/ai-hat/)
(Hailo-8L, 13 TOPS). Builds on any Linux x86-64 or AArch64 machine.

---

## Pipeline

```
VOICE_ASSIST_TRIGGER (gRPC / vhal-core)  ─or─  Wake word (mic)
  → Microphone (PortAudio)
  → ITranscriber:
      • WhisperTranscriber  (whisper.cpp — CPU / CUDA)
      • HailoTranscriber    (HailoRT NPU — Hailo-8L on RPi AI HAT+)
  → Ollama       (LLM — runs on host PC when deployed to RPi)
  → Piper        (TTS)
  → Speaker      (PortAudio)
```

Velan is a **gRPC client** of [vhal-core](https://github.com/aananthcn/vhal-core),
the Linux port of Android VHAL. vhal-core must be built and deployed to the
target before Velan can run.

---

## Prerequisites

### 1. System packages (build tools only)

C++ library dependencies (gRPC, Protobuf, PortAudio, libcurl, whisper.cpp) are
managed by Conan — no system-level dev packages needed for them.

**Debian / Ubuntu:**

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    python3-pip
```

**Fedora / RHEL:**

```bash
sudo dnf install -y gcc-c++ cmake ninja-build git python3-pip
```

### 2. Conan (required for both vhal-core and Velan)

Both vhal-core and Velan use Conan 2.x to manage C++ dependencies. This
eliminates tool/library version drift between the build machine and the target.

```bash
pip3 install conan
conan profile detect          # create default profile once per machine
```

### 3. AArch64 cross-compiler (rpi target only)

Required to cross-compile on a PC for the Raspberry Pi. Library headers come
from Conan — no arm64 multiarch packages needed.

```bash
sudo apt install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

> **First rpi build:** Conan builds gRPC and its dependencies (abseil, re2,
> c-ares) from source for aarch64. This takes 30–60 min. Subsequent builds
> use the `~/.conan2` binary cache and are fast.

### 4. Ollama

Ollama serves the LLM locally over a REST API.

```bash
curl -fsSL https://ollama.com/install.sh | sh
```

Pull a model suited to the target board's RAM:

```bash
ollama pull llama3.2:3b   # good balance of speed and quality on RPi5
ollama pull llama3.2:1b   # fastest option
```

Ollama starts automatically as a systemd service. To verify:

```bash
systemctl status ollama
```

#### Ollama for RPi deployment

When Velan runs on the RPi it must reach Ollama on the **host PC** over LAN.
The systemd Ollama service binds to `127.0.0.1` by default — the RPi cannot
reach it. Use the provided helper script to restart Ollama bound to all
interfaces while preserving GPU and flash-attention settings:

```bash
./scripts/restart_ollama4rpi.sh
```

This script stops the systemd service (`sudo` required once for the password),
then starts Ollama with `OLLAMA_HOST=0.0.0.0` and the correct `OLLAMA_MODELS`
path so all previously downloaded models remain visible.

> **Note:** `run_velan.sh --target rpi` auto-injects `--llm <host_ip>` so the
> RPi velan binary always points at the right host.  Run
> `restart_ollama4rpi.sh` on the PC **before** launching `run_velan.sh`.

### 5. RPi runtime packages

The following packages must be installed **on the Raspberry Pi** (not on the
build PC). They are runtime dependencies — not needed to build Velan.

```bash
# On the Raspberry Pi:
sudo apt install -y pulseaudio-utils
```

| Package | Why |
|---------|-----|
| `pulseaudio-utils` | Provides `paplay`, used by Velan's TTS fallback to route audio through PipeWire to a Bluetooth speaker. Required because PortAudio (conan build) cannot open the RPi ALSA device directly. |

> **`pulseaudio-utils` only — do not install `pulseaudio` or `libasound2-plugins`.**
>
> - `pulseaudio-utils` ships only CLI tools (`paplay`, `pactl`, …) — no daemon,
>   no ALSA config changes. `paplay` connects to PipeWire's PulseAudio socket
>   directly via `libpulse0`, bypassing ALSA entirely.
> - Installing the full `pulseaudio` package pulls in `libasound2-plugins` as a
>   dependency. That package redirects the ALSA `default` device to PulseAudio,
>   which causes PortAudio's `Pa_Initialize()` to crash on startup (the ALSA
>   pulse plugin conflicts with PipeWire's audio ownership).
> - If you accidentally installed `pulseaudio`, recover with:
>   ```bash
>   sudo apt install pulseaudio-utils   # keep paplay
>   sudo apt remove pulseaudio libasound2-plugins
>   sudo apt autoremove
>   ```

### 6. AI models and Piper TTS binary

```bash
./scripts/download_models.sh
```

Downloads everything in one step (skips files that already exist):

| File | Destination | Purpose |
|------|-------------|---------|
| `ggml-medium.bin` | `models/stt/` | Whisper STT + wake-word model |
| `en_US-lessac-medium.onnx` | `models/tts/` | Piper voice model |
| `en_US-lessac-medium.onnx.json` | `models/tts/` | Piper voice config |
| `piper` binary + libs | `~/.local/lib/piper/` | Piper TTS executable |

Alternative Whisper models (trade accuracy for speed):

| Model | Size | RPi5 latency | Notes |
|-------|------|--------------|-------|
| `ggml-tiny.bin` | 75 MB | ~1 s | Low accuracy |
| `ggml-base.bin` | 142 MB | ~2 s | Decent accuracy |
| `ggml-small.bin` | 466 MB | ~4 s | Good accuracy |
| `ggml-medium.bin` | 1.5 GB | ~8–12 s | **Default** |
| `ggml-large-v3.bin` | 3.1 GB | >30 s | Too slow on CPU |

---

## Build & Deploy Workflow

All scripts accept `--target pc` (local x86-64) or `--target rpi` (Raspberry Pi 5).
The workflow is identical for both targets; only the target flag and optional
`--ip` / `--user` arguments differ.

```
┌─────────────────────────────────────────────────────────┐
│  Step 1 & 2 — vhal-core (dependency, build once)        │
│  build_vhal_core.sh  →  deploy_vhal_core.sh             │
├─────────────────────────────────────────────────────────┤
│  Step 3 & 4 — Velan (repeat on every code change)       │
│  build_velan.sh  →  deploy_velan.sh                     │
├─────────────────────────────────────────────────────────┤
│  Step 5 — Run                                           │
│  run_velan.sh                                           │
└─────────────────────────────────────────────────────────┘
```

Both vhal-core and Velan are deployed to `/opt/car-ui/` on the target:

```
/opt/car-ui/
├── bin/
│   ├── vhal-core       VHAL gRPC server
│   ├── vhal-gateway    Property forwarding daemon
│   ├── velan           Velan voice assistant
│   └── piper           Piper TTS wrapper
├── lib/
│   └── piper/          Piper binary + shared libs
├── models/
│   ├── stt/            Whisper model files
│   └── tts/            Piper voice model files
└── etc/
    └── vhal/           vhal-core and gateway config files
```

---

### Quick start — do_all.sh

`do_all.sh` runs the full **build → deploy → run** chain for Velan in one command.
It does not touch vhal-core — build that once with steps 1–2 below.

Default AI accelerators: **CUDA** for `--target pc`, **Hailo-8** for `--target rpi`.
The deploy confirmation prompt is auto-answered.

```bash
# PC — full build + deploy + run (CUDA by default)
./scripts/do_all.sh --target pc

# PC — override to CPU, pass a specific model to velan
./scripts/do_all.sh --target pc --aicore cpu --sttmodel models/stt/ggml-tiny.bin

# RPi — full build + deploy + run (Hailo-8 by default)
./scripts/do_all.sh --target rpi --ip 192.168.10.30

# RPi — with remote LLM and custom wake phrase
./scripts/do_all.sh --target rpi --ip 192.168.10.30 --llm 192.168.10.10 --wwphrase "Hey Vela,Subramanya"
```

| Argument | Required | Description |
|----------|----------|-------------|
| `--target <pc\|rpi>` | Yes | Build and deploy target |
| `--aicore <cpu\|cuda\|hailo8>` | No | AI accelerator (default: `cuda` for pc, `hailo8` for rpi) |
| `--ip <addr>` | rpi only | RPi IP address (default: `192.168.10.30`) |
| `--user <name>` | No | RPi login user (default: `$USER`) |
| `-j <jobs>` | No | Parallel build jobs (default: `nproc`) |
| `[VELAN OPTIONS]` | No | Extra args forwarded to the velan binary |

---

### Step 1 — Build vhal-core

vhal-core uses Conan + CMake. The script installs Conan dependencies,
configures, builds, and stages the install tree under
`<vhal-core>/build/<target>/install/`.

```bash
./scripts/build_vhal_core.sh --target pc        # native x86-64
./scripts/build_vhal_core.sh --target rpi       # cross-compiled AArch64
```

The vhal-core source is expected at `../../networking/vhal-core` relative to
this repo. Override with:

```bash
VHAL_CORE_DIR=/path/to/vhal-core ./scripts/build_vhal_core.sh --target rpi
```

---

### Step 2 — Deploy vhal-core

Copies the staged vhal-core install tree to `/opt/car-ui/` on the target.

**PC (local deploy, requires sudo):**

```bash
./scripts/deploy_vhal_core.sh --target pc
```

**Raspberry Pi (rsync over SSH):**

```bash
./scripts/deploy_vhal_core.sh --target rpi
./scripts/deploy_vhal_core.sh --target rpi --ip 192.168.10.30 --user pi
```

Deploys:
- `bin/vhal-core` — VHAL gRPC server
- `bin/vhal-gateway` — property forwarding daemon
- `etc/vhal/vhalconfig/DefaultProperties.json` — VHAL property defaults
- `etc/vhal/gateway-configs.json` — gateway forwarding rules

---

### Step 3 — Build Velan

Velan uses Conan + CMake. All C++ dependencies — including gRPC, Protobuf,
PortAudio, libcurl, nlohmann/json, and whisper.cpp — are fetched and built
by Conan automatically. No system-level dev packages or manual downloads needed.

```bash
./scripts/build_velan.sh --target pc                    # PC, CPU only
./scripts/build_velan.sh --target pc --aicore cuda      # PC, CUDA acceleration
./scripts/build_velan.sh --target rpi                   # Raspberry Pi, CPU only
./scripts/build_velan.sh --target rpi --aicore hailo8   # Raspberry Pi, Hailo-8
```

| Argument | Required | Values | Description |
|----------|----------|--------|-------------|
| `--target` | Yes | `pc`, `rpi` | Build target platform |
| `--aicore` | No | `cpu`, `cuda`, `hailo8` | AI accelerator (default: `cuda` for pc, `hailo8` for rpi) |
| `-j` | No | number | Parallel jobs (default: `nproc`) |

Output binary: `build/<target>/velan`

**CUDA on PC** requires CUDA Toolkit 12.8+:

```bash
nvcc --version    # must say 12.8 or later
nvidia-smi        # confirm GPU is visible
```

Install CUDA 12.8 on Ubuntu 22.04:

```bash
sudo apt remove --purge 'cuda*' 'libcuda*' 'nvidia-cuda*' && sudo apt autoremove
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt update && sudo apt install -y cuda-toolkit-12-8
export PATH=/usr/local/cuda-12.8/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda-12.8/lib64:$LD_LIBRARY_PATH
```

---

### Step 4 — Deploy Velan

Copies the Velan binary, Piper TTS, and AI models to `/opt/car-ui/` on the
target. Run `download_models.sh` first if you have not already.

**PC (local deploy, requires sudo):**

```bash
./scripts/deploy_velan.sh --target pc
```

**Raspberry Pi (rsync over SSH):**

```bash
./scripts/deploy_velan.sh --target rpi
./scripts/deploy_velan.sh --target rpi --ip 192.168.10.30 --user pi
```

Deploys:
- `bin/velan` — Velan binary
- `bin/piper` + `lib/piper/` — Piper TTS wrapper and shared libs
- `models/stt/` — Whisper model files
- `models/tts/` — Piper voice model files

---

### Step 5 — Run

Starts vhal-core and Velan from `/opt/car-ui/`. Both must be deployed first.

**PC:**

```bash
./scripts/run_velan.sh --target pc
```

**Raspberry Pi** (vhal-core runs locally; Velan runs on the RPi over SSH):

```bash
./scripts/run_velan.sh --target rpi
./scripts/run_velan.sh --target rpi --ip 192.168.10.30 --user pi
```

For the RPi target, the script **auto-detects the host PC's LAN IP** and passes
`--llm <host_ip>` to velan so it can reach Ollama on the PC.  Override with
`--llm-host <addr>` if auto-detect picks the wrong interface.

Ctrl-C stops all processes cleanly on both sides.

Extra arguments are forwarded to the Velan binary:

```bash
./scripts/run_velan.sh --target pc --wwphrase "Hey Vela" --llmodel llama3.2:1b
```

---

### Full example — fresh PC setup

```bash
# 1. Download AI models and Piper binary
./scripts/download_models.sh

# 2. Build and deploy vhal-core
./scripts/build_vhal_core.sh  --target pc
./scripts/deploy_vhal_core.sh --target pc

# 3. Build and deploy Velan (with CUDA)
./scripts/build_velan.sh  --target pc --aicore cuda
./scripts/deploy_velan.sh --target pc

# 4. Run
./scripts/run_velan.sh --target pc
```

### Full example — fresh Raspberry Pi setup

```bash
# 1. Download AI models and Piper binary (on the PC)
./scripts/download_models.sh

# 2. (Hailo-8 only, one time) Extract NPU embedding weights + vocab
pip install openai-whisper
python3 scripts/extract_whisper_weights.py --model tiny \
        --out models/stt/whisper-tiny-h8l/weights

# 3. Build and deploy vhal-core (cross-compiled on PC, deployed to RPi)
./scripts/build_vhal_core.sh  --target rpi
./scripts/deploy_vhal_core.sh --target rpi --ip 192.168.10.30

# 4. Build and deploy Velan (cross-compiled on PC, deployed to RPi)
./scripts/build_velan.sh  --target rpi --aicore hailo8
./scripts/deploy_velan.sh --target rpi --ip 192.168.10.30

# 5. Run (vhal-core on PC, Velan on RPi via SSH)
./scripts/run_velan.sh --target rpi --ip 192.168.10.30
```

---

## Usage

Velan waits for `VOICE_ASSIST_TRIGGER` events from the vhal-core gRPC server.
VAD (voice activity detection) starts and stops recording automatically.

| Option | Default | Description |
|--------|---------|-------------|
| `--aicore <mode>` | `whisper` | Transcription backend: `whisper` (CPU/CUDA) or `hailo8` (NPU) |
| `--sttmodel <path>` | `models/stt/ggml-medium.bin` | Whisper model (whisper aicore only) |
| `--encoder-hef <path>` | `models/stt/whisper-tiny-h8l/encoder.hef` | Hailo encoder HEF |
| `--decoder-hef <path>` | `models/stt/whisper-tiny-h8l/decoder.hef` | Hailo decoder HEF |
| `--vocab-json <path>` | `models/stt/whisper-tiny-h8l/weights/vocab.json` | Hailo vocab file |
| `--ttsmodel <path>` | `models/tts/en_US-lessac-medium.onnx` | Piper TTS model |
| `--llmodel <name>` | `llama3.2:3b` | Ollama model name |
| `--llm <host>` | `localhost` | Ollama server hostname or IP |
| `--server <host:port>` | `localhost:50051` | VHAL gRPC server address |
| `--wwphrase <phrases>` | `Hey Vela, Subramanya, Subramani, Subrahmanya` | Comma-separated wake phrases |
| `--mic <index>` | system default | PortAudio input device index |
| `--list-mic` | — | Print available microphone devices and exit |
| `--tts-sink <device>` | system default | ALSA device for TTS `aplay` fallback (e.g. `plughw:0,0`). Use `pulse` to route via PulseAudio/PipeWire — required for Bluetooth speakers on RPi. Auto-injected by `run_velan.sh --target rpi`. |
| `--test-wav <path>` | — | Transcribe a WAV file (16 kHz mono) and exit; no mic/LLM/TTS needed |

---

## Testing with the Python trigger client

`test/trigger_velan.py` simulates a hardware button by sending
`VOICE_ASSIST_TRIGGER` to vhal-core via gRPC `SetValues`.

Install Python deps once:

```bash
pip install grpcio grpcio-tools
```

With `run_velan.sh --target pc` already running, open a second terminal:

```bash
python3 test/trigger_velan.py
```

Press **Enter** to trigger, **Backspace/ESC** to send off, **Ctrl-C** to quit.

Velan output:

```
[Velan] TRIGGER_ON  — recording started.
[Velan] Transcribing...
[Velan] You said:  What is the capital of Tamil Nadu?
[Velan] Thinking...
[Velan] Assistant: Chennai.
```

Conversation history is preserved across turns within a session.

---

## Hailo-8L NPU acceleration

When `--aicore hailo8` is used, Velan runs Whisper inference on the Hailo-8L
NPU via HailoRT instead of whisper.cpp. The pipeline is:

```
PCM audio → log-mel (CPU) → encoder.hef (NPU) → decoder.hef (NPU) → tokens → text
```

The decoder HEF uses an **input-split** architecture: the token embedding
lookup is computed on the CPU (using `.npy` weight files) and the full
transformer + output projection runs on the NPU. This keeps the large embedding
table off the NPU while still offloading all attention layers.

### One-time: extract NPU weights + mel filterbank

Before the first Hailo build, run the extraction script **once** on any machine
with Python installed. It downloads the Whisper tiny model, extracts the
weight tensors the CPU embedding step needs, the exact mel filterbank used when
the ONNX/HEF was compiled, and generates the vocabulary file:

```bash
pip install openai-whisper   # also installs numpy
python3 scripts/extract_whisper_weights.py --model tiny \
        --out models/stt/whisper-tiny-h8l/weights
```

This creates four files (re-run only if you change the model size):

| File | Size | Purpose |
|------|------|---------|
| `token_embedding_weight_tiny.npy` | ~75 MB | Decoder input embedding `[vocab × 384]` |
| `onnx_add_input_tiny.npy` | ~48 KB | Positional encoding `[32 × 384]` |
| `vocab.json` | ~1 MB | Token-ID → UTF-8 string mapping |
| `mel_filters_80.npy` | ~64 KB | Whisper's exact mel filterbank `[80 × 201]` |

`deploy_velan.sh` copies the whole `models/` tree to the RPi, so these files
are automatically deployed alongside the HEF files.

> **Adjust `--model` to match the HEF.** If the encoder/decoder HEFs were
> compiled from `whisper-base`, run `--model base`; for `whisper-small`, use
> `--model small`. Mismatched sizes cause decoder repetition loops.

### Testing with a known WAV (offline, no mic needed)

`scripts/test_hailo_wav.sh` generates a TTS WAV on the host, copies it to the
RPi, and runs `velan --test-wav` to transcribe it:

```bash
./scripts/test_hailo_wav.sh --ip 192.168.10.30 --text "Hello Velan"
```

This validates the full Hailo pipeline (mel → encoder → decoder → text) without
needing a working microphone or Ollama.

---

## Troubleshooting

**`velan` or `vhal-core` not found at `/opt/car-ui/bin/`**

```bash
./scripts/deploy_vhal_core.sh --target pc   # or --target rpi
./scripts/deploy_velan.sh     --target pc   # or --target rpi
```

**No audio input detected**

```bash
pactl list sources short
pactl set-default-source <source-name>
```

**Whisper model fails to load**

```bash
ls -lh models/stt/
./scripts/download_models.sh   # re-downloads missing files, skips existing
```

**Piper not found or TTS silent**

```bash
./scripts/download_models.sh   # installs Piper to ~/.local/lib/piper/
./scripts/deploy_velan.sh --target pc   # redeploys piper to /opt/car-ui/
```

**Ollama connection refused (PC target)**

```bash
ollama serve
```

**Ollama not reachable from RPi** (`curl: Couldn't connect to server`)

By default Ollama binds to `127.0.0.1` only. The RPi cannot reach it over LAN.
Use the helper script (run on the **PC**):

```bash
./scripts/restart_ollama4rpi.sh
```

This stops the systemd service and restarts Ollama with `OLLAMA_HOST=0.0.0.0`
and the correct `OLLAMA_MODELS` path (preserves GPU / flash-attention settings).

`run_velan.sh` auto-injects `--llm <host_ip>` for the rpi target — you do not
need to pass it manually unless the auto-detect picks the wrong interface.

**Conan `Invalid` error during `build_vhal_core.sh`**

Ensure `compiler.cppstd=17` is set in the Conan profiles at
`<vhal-core>/profiles/linux-x86` and `<vhal-core>/profiles/rpi5-linux`.

**Slow transcription on RPi5**

Switch to a smaller model in `run_velan.sh`:

```bash
./scripts/run_velan.sh --target rpi --sttmodel /opt/car-ui/models/stt/ggml-small.bin
```

---

## Project layout

```
velan/
├── src/
│   ├── main.cpp                    VHAL gRPC loop, CLI, signal handling, init order
│   ├── Transcriber.h               ITranscriber interface (pure virtual)
│   ├── WhisperTranscriber.h/.cpp   ITranscriber via whisper.cpp (CPU/CUDA)
│   ├── HailoTranscriber.h/.cpp     ITranscriber via HailoRT NPU (Hailo-8L)
│   ├── WakeWordDetector.h/.cpp     Onset-triggered wake-word detection
│   ├── Speech2TextManager.h/.cpp   Singleton: record → transcribe → on_transcript
│   ├── TransformerManager.h/.cpp   Ollama multi-turn conversation + HTTP chat
│   └── Text2SpeechManager.h/.cpp   Piper TTS subprocess + PortAudio playback
├── conan/
│   ├── recipes/whisper/
│   │   └── conanfile.py            Local Conan recipe for whisper.cpp v1.7.4
│   └── recipes/portaudio/
│       └── conanfile.py            Local Conan recipe for PortAudio (ALSA)
├── profiles/
│   ├── pc                          Conan profile: x86_64 native
│   └── rpi                         Conan profile: armv8 / aarch64 cross-compile
├── scripts/
│   ├── build_vhal_core.sh          Build vhal-core (Conan + CMake, pc or rpi)
│   ├── deploy_vhal_core.sh         Deploy vhal-core to /opt/car-ui/
│   ├── build_velan.sh              Build Velan (Conan + CMake, pc or rpi, optional aicore)
│   ├── deploy_velan.sh             Deploy Velan binary, Piper, models to /opt/car-ui/
│   ├── run_velan.sh                Start vhal-core + Velan; auto-injects --llm and --tts-sink for RPi
│   ├── restart_ollama4rpi.sh       Restart Ollama on PC bound to 0.0.0.0 (required for RPi LAN access)
│   ├── do_all.sh                   build → deploy → run in one command
│   ├── download_models.sh          Download Whisper models and Piper binary
│   ├── download_hailo8_models.sh   Check/download Hailo HEF files
│   ├── extract_whisper_weights.py  One-time: extract NPU weights + mel filterbank + vocab
│   ├── test_hailo_wav.sh           Generate TTS WAV + run --test-wav on RPi
│   └── sync_rpi_sysroot.sh         Sync RPi sysroot to ~/sdk/rpi/adas
├── models/
│   ├── stt/                        Whisper model files (gitignored)
│   │   └── whisper-tiny-h8l/
│   │       ├── encoder.hef         Hailo encoder (gitignored)
│   │       ├── decoder.hef         Hailo decoder (gitignored)
│   │       └── weights/            NPU weight files (gitignored)
│   └── tts/                        Piper voice model files (gitignored)
├── build/
│   ├── pc/                         PC build output
│   └── rpi/                        RPi cross-compiled build output
├── test/
│   └── trigger_velan.py            Python gRPC client — sends VOICE_ASSIST_TRIGGER
├── conanfile.py                    Root Conan file: all C++ dependencies
├── CMakeLists.txt
├── CLAUDE.md                       Design decisions + implementation constraints
├── ARCHITECTURE.md                 System architecture and component overview
└── README.md
```

Proto files are consumed directly from
`~/labs/networking/vhal-core/` — no duplication in this repo.
