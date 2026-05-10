# Velan (C++)

A local Automotive assistant that recognises drivers, listens to voice commands, transcribes it with [whisper.cpp](https://github.com/ggerganov/whisper.cpp), and sends the text to a local LLM via [Ollama](https://ollama.com). No cloud APIs, no Python runtime required at execution time.

Developed on a **PC with Nvidia GPU** (RTX 5070), then deployed to **Raspberry Pi 5** with the [Raspberry Pi AI HAT+](https://www.raspberrypi.com/products/ai-hat/) (Hailo-8L, 13 TOPS). Builds on any Linux x86-64 or AArch64 machine.

---

## Pipeline

```
VOICE_ASSIST_TRIGGER (gRPC) → Microphone → PortAudio → whisper.cpp (STT) → Ollama (LLM) → Terminal
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

Pull a model suited to the RPi5's RAM (8 GB recommended):

```bash
# 3B — good balance of speed and quality on RPi5
ollama pull llama3.2:3b

# 1B — fastest option
ollama pull llama3.2:1b
```

Ollama starts automatically as a systemd service after install. To check:

```bash
systemctl status ollama
# or start manually:
ollama serve
```

### 3. Whisper GGML model

whisper.cpp uses its own quantised `.bin` model format. Download directly from
Hugging Face (the `bash <(curl ...)` process-substitution method fails on some
Linux kernels because the helper script tries to `cd` to its own `/proc` path):

```bash
mkdir -p models
curl -L --progress-bar \
  -o models/ggml-medium.bin \
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin"
```

Substitute `ggml-medium.bin` with any name from the table below:

| Model | File | Size on disk | RPi5 latency (approx) | Notes |
|---|---|---|---|---|
| `tiny` | `ggml-tiny.bin` | 75 MB | ~1 s | Low accuracy |
| `base` | `ggml-base.bin` | 142 MB | ~2 s | Decent accuracy |
| `small` | `ggml-small.bin` | 466 MB | ~4 s | Good accuracy |
| `medium` | `ggml-medium.bin` | 1.5 GB | ~8–12 s | **Recommended** |
| `large-v3` | `ggml-large-v3.bin` | 3.1 GB | >30 s | Too slow on CPU |

The model file will be saved to `models/ggml-<name>.bin`.

---

## Build

CMake fetches whisper.cpp and nlohmann/json automatically at configure time — no manual submodule steps needed.

### PC with Nvidia GPU (primary development target)

Requires **CUDA Toolkit 12.8+**. The RTX 5070 (Blackwell/sm_120) is not
supported by older toolkits, and CUDA 11.x is incompatible with GCC 11.

Verify what is installed:

```bash
nvcc --version    # must say 12.8 or later
nvidia-smi        # confirm GPU is visible
```

Install or upgrade to CUDA 12.8 on Ubuntu 22.04:

```bash
# Remove old CUDA if present
sudo apt remove --purge 'cuda*' 'libcuda*' 'nvidia-cuda*' && sudo apt autoremove

# Add NVIDIA package repository
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt update
sudo apt install -y cuda-toolkit-12-8

# Add to PATH (also add to ~/.bashrc)
export PATH=/usr/local/cuda-12.8/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda-12.8/lib64:$LD_LIBRARY_PATH
```

Configure and build with CUDA enabled:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON
cmake --build build -j$(nproc)
```

whisper.cpp will offload Whisper inference to the GPU. On an RTX 5070 expect
transcription in under 1 second for most utterances.

### Raspberry Pi 5 + AI HAT (deployment target)

No CUDA on RPi — the build uses ARM NEON automatically:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

The Hailo-8L accelerator (13 TOPS) is not used by whisper.cpp; see the
**Hailo AI HAT+ note** section below.

### Optional: install system-wide

```bash
sudo cmake --install build
# binary installed to /usr/local/bin/velan
```

---

## Usage

Velan no longer uses keyboard input. It waits for `VOICE_ASSIST_TRIGGER` events
from a gRPC server (the ECU — IVI, Cluster, or ADAS). Recording starts on
`TRIGGER_ON` and stops on `TRIGGER_OFF`.

```bash
./build/velan [whisper_model_path] [ollama_model] [grpc_server]
```

| Argument | Default | Example |
|---|---|---|
| `whisper_model_path` | `models/ggml-medium.bin` | `models/ggml-small.bin` |
| `ollama_model` | `llama3.2` | `llama3.2:3b` |
| `grpc_server` | `localhost:50055` | `192.168.1.10:50055` |

### Testing with the Python trigger server

`test/trigger_velan.py` is a gRPC client that writes the `VOICE_ASSIST_TRIGGER`
property to VHAL core via `SetValues`.  VHAL core must already be running.

Install the Python deps once:

```bash
pip install grpcio grpcio-tools
```

Start VHAL core (Terminal 1 — see vhal-core repo):

```bash
./vhal-server
```

Start Velan (Terminal 2):

```bash
./build/velan
```

In Terminal 3, type `on` to begin recording and `off` to stop and get a response:

```bash
python3 test/trigger_velan.py
```

```
Connected to VHAL server at localhost:50051
Commands: on | off | quit

trigger> on
  → TRIGGER_ON  sent  (status: [0])
trigger> off
  → TRIGGER_OFF sent  (status: [0])
trigger>
```

Velan output in Terminal 2:

```
[Velan] Loading Whisper model (CPU): models/ggml-medium.bin
[Velan] Whisper model loaded.
[Velan] Ollama model: llama3.2
[Velan] Connecting to trigger server at localhost:50055
[Velan] Subscribed to VOICE_ASSIST_TRIGGER events. Waiting...
[Velan] TRIGGER_ON  — recording started.
[Velan] TRIGGER_OFF — recording stopped.
[Velan] Transcribing...
[Velan] You said:  What is the capital of France?
[Velan] Thinking...
[Velan] Assistant: The capital of France is Paris.
```

Conversation history is preserved across turns within a session.
Velan reconnects automatically if the trigger server restarts.

---

## Hailo AI HAT+ note

The Hailo-8L accelerator (13 TOPS) does not natively run whisper.cpp — it requires
models compiled through [Hailo Dataflow Compiler](https://hailo.ai/developer-zone/documentation/)
into Hailo Executable Format (HEF). whisper.cpp runs on the RPi5 CPU instead, using
ARM NEON SIMD automatically. This is sufficient for interactive use with `small` or
`medium` models.

Hailo HAT acceleration for Whisper would be a separate integration step outside the
scope of this project.

---

## Troubleshooting

**No audio input detected**

List available devices and confirm your microphone appears:

```bash
pactl list sources short
```

Set the default source if needed:

```bash
pactl set-default-source <source-name>
```

**Whisper model fails to load**

Confirm the file exists and is not zero bytes:

```bash
ls -lh models/
```

Re-download using the `curl` command in the **Whisper GGML model** section above.

**Ollama connection refused**

Ensure Ollama is running before starting the assistant:

```bash
ollama serve
```

By default the assistant connects to `http://localhost:11434`. If Ollama runs on
another host, set `OLLAMA_CHAT_URL` in `src/main.cpp` and rebuild.

**Slow transcription on RPi5**

Switch to a smaller model:

```bash
./build/velan models/ggml-small.bin llama3.2:1b
```

---

## Project layout

```
velan/
├── src/
│   └── main.cpp          # Full pipeline: VHAL trigger → audio → Whisper → Ollama
├── models/               # GGML model files (gitignored, download separately)
├── test/
│   └── trigger_velan.py  # Python gRPC client — writes VOICE_ASSIST_TRIGGER to VHAL
├── CMakeLists.txt        # Generates C++ stubs from vhal-core proto files
└── README.md
```

Proto files are consumed directly from `~/labs/networking/vhal-core/test/vhal/`
— no duplication in this repo.
