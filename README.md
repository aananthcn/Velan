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
VOICE_ASSIST_TRIGGER (gRPC)
  → Microphone (PortAudio)
  → whisper.cpp  (STT)
  → Ollama       (LLM)
  → Piper        (TTS)
  → Speaker      (PortAudio)
```

---

## Prerequisites

### 1. System packages

**Raspberry Pi OS (Bookworm) / Debian / Ubuntu:**

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    pkg-config \
    portaudio19-dev \
    libcurl4-openssl-dev \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc
```

**Fedora / RHEL:**

```bash
sudo dnf install -y \
    gcc-c++ \
    cmake \
    ninja-build \
    git \
    pkgconf \
    portaudio-devel \
    libcurl-devel \
    grpc-devel \
    grpc-plugins \
    protobuf-devel
```

### 2. Ollama

Ollama serves the LLM locally over a REST API.

```bash
curl -fsSL https://ollama.com/install.sh | sh
```

Pull a model suited to the target board's RAM (8 GB recommended):

```bash
ollama pull llama3.2:3b   # good balance of speed and quality on RPi5
ollama pull llama3.2:1b   # fastest option
```

Ollama starts automatically as a systemd service after install. To check:

```bash
systemctl status ollama
# or start manually:
ollama serve
```

### 3. Piper TTS

Piper is not in the standard apt repositories (except Ubuntu 24.04+). The
`scripts/download_models.sh` script handles downloading the correct prebuilt
binary for your architecture (x86\_64, aarch64, or armv7l) along with all
model files — see the **Models** section below.

If you are on **Ubuntu 24.04+** and prefer apt:

```bash
sudo apt install piper-tts
```

Otherwise, run the download script and it will install the binary under
`bin/piper/` in the project directory. `test/run_velan.sh` adds this to
`PATH` automatically.

### 4. Models

Download all required model files and the Piper binary in one step:

```bash
./scripts/download_models.sh
```

This downloads (and skips any file that already exists):

| File | Destination | Purpose |
|------|-------------|---------|
| `ggml-medium.bin` | `models/stt/` | Whisper STT model |
| `en_US-lessac-medium.onnx` | `models/tts/` | Piper voice model |
| `en_US-lessac-medium.onnx.json` | `models/tts/` | Piper voice config |
| `piper` binary + libs | `bin/piper/` | Piper TTS executable |

Alternative Whisper models (trade accuracy for speed):

| Model | Size | RPi5 latency | Notes |
|-------|------|--------------|-------|
| `ggml-tiny.bin` | 75 MB | ~1 s | Low accuracy |
| `ggml-base.bin` | 142 MB | ~2 s | Decent accuracy |
| `ggml-small.bin` | 466 MB | ~4 s | Good accuracy |
| `ggml-medium.bin` | 1.5 GB | ~8–12 s | **Default** |
| `ggml-large-v3.bin` | 3.1 GB | >30 s | Too slow on CPU |

---

## Build

CMake fetches whisper.cpp and nlohmann/json automatically at configure time —
no manual submodule steps needed.

Use the build script for a parallel build:

```bash
./scripts/build.sh            # CPU build
./scripts/build.sh --cuda     # GPU build (requires CUDA Toolkit 12.8+)
./scripts/build.sh --cuda -j4 # GPU build, 4 cores
```

### PC with Nvidia GPU (primary development target)

Requires **CUDA Toolkit 12.8+**. Verify:

```bash
nvcc --version    # must say 12.8 or later
nvidia-smi        # confirm GPU is visible
```

Install or upgrade to CUDA 12.8 on Ubuntu 22.04:

```bash
sudo apt remove --purge 'cuda*' 'libcuda*' 'nvidia-cuda*' && sudo apt autoremove

wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt update
sudo apt install -y cuda-toolkit-12-8

export PATH=/usr/local/cuda-12.8/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda-12.8/lib64:$LD_LIBRARY_PATH
```

### Raspberry Pi 5 (deployment target)

No CUDA on RPi — ARM NEON is used automatically:

```bash
./scripts/build.sh
```

### Optional: install system-wide

```bash
sudo cmake --install build
# binary installed to /usr/local/bin/velan
```

---

## Usage

Velan waits for `VOICE_ASSIST_TRIGGER` events from a VHAL gRPC server.
Recording starts on `TRIGGER_ON` and stops on `TRIGGER_OFF`, after which the
captured audio is transcribed, sent to the LLM, and the reply is spoken aloud.

```bash
./build/velan [OPTIONS]
```

| Option | Default | Example |
|--------|---------|---------|
| `--vmodel <path>` | `models/stt/ggml-medium.bin` | `models/stt/ggml-small.bin` |
| `--tts <path>` | `models/tts/en_US-lessac-medium.onnx` | `models/tts/en_GB-jenny-medium.onnx` |
| `--llm <model>` | `llama3.2:3b` | `gemma4:26b` |
| `--server <host:port>` | `localhost:50051` | `192.168.1.10:50051` |

### Quick start

```bash
# Download models and Piper binary (once)
./scripts/download_models.sh

# Build (once)
./scripts/build.sh

# Run (starts vhal-core and velan together, Ctrl+C stops both)
./test/run_velan.sh
```

### Testing with the Python trigger client

`test/trigger_velan.py` is a gRPC client that writes `VOICE_ASSIST_TRIGGER`
to VHAL core via `SetValues`.

Install Python deps once:

```bash
pip install grpcio grpcio-tools
```

With `test/run_velan.sh` already running, open a second terminal:

```bash
python3 test/trigger_velan.py
```

```
Commands: on | off | quit

trigger> on
  → TRIGGER_ON  sent
trigger> off
  → TRIGGER_OFF sent
```

Velan output:

```
[Velan] TRIGGER_ON  — recording started.
[Velan] TRIGGER_OFF — recording stopped.
[Velan] Transcribing...
[Velan] You said:  What is the capital of Tamil Nadu?
[Velan] Thinking...
[Velan] Assistant: Chennai.
```

Followed by Piper speaking the reply through the default audio output.
Conversation history is preserved across turns within a session.

---

## Hailo AI HAT+ note

The Hailo-8L accelerator (13 TOPS) does not natively run whisper.cpp — it
requires models compiled through
[Hailo Dataflow Compiler](https://hailo.ai/developer-zone/documentation/)
into Hailo Executable Format (HEF). whisper.cpp runs on the RPi5 CPU instead,
using ARM NEON SIMD automatically. This is sufficient for interactive use with
`small` or `medium` models.

Hailo HAT acceleration for Whisper would be a separate integration step outside
the scope of this project.

---

## Troubleshooting

**No audio input detected**

```bash
pactl list sources short
pactl set-default-source <source-name>
```

**Whisper model fails to load**

```bash
ls -lh models/stt/
./scripts/download_models.sh   # re-downloads missing files, skips existing ones
```

**Piper not found or TTS silent**

```bash
./scripts/download_models.sh   # downloads Piper binary to ~/.local/bin/piper
# Always launch via test/run_velan.sh — it sets PATH and LD_LIBRARY_PATH
```

**Ollama connection refused**

```bash
ollama serve
```

**Slow transcription on RPi5**

```bash
./build/velan --vmodel models/stt/ggml-small.bin --llm llama3.2:1b
```

---

## Project layout

```
velan/
├── src/
│   ├── main.cpp                    VHAL gRPC polling loop, CLI, signal handling
│   ├── Speech2TextManager.h/.cpp   Singleton: PortAudio capture + Whisper STT
│   ├── TransformerManager.h/.cpp   Ollama conversation history + HTTP chat
│   └── Text2SpeechManager.h/.cpp   Piper TTS subprocess + PortAudio playback
├── scripts/
│   ├── build.sh                    Configure + parallel build (Ninja, --cuda flag)
│   └── download_models.sh          Download Whisper model, Piper voice + binary
├── models/
│   ├── stt/                        Whisper model files (gitignored)
│   └── tts/                        Piper voice model files (gitignored)
├── test/
│   ├── trigger_velan.py            Python gRPC client — sends VOICE_ASSIST_TRIGGER
│   └── run_velan.sh                Starts vhal-core + velan, Ctrl+C stops both
├── CMakeLists.txt
├── ARCHITECTURE.md
└── README.md
```

Proto files are consumed directly from
`~/labs/networking/vhal-core/test/vhal/` — no duplication in this repo.
