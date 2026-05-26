// Copyright 2026 Aananth C N
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "Text2SpeechManager.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <portaudio.h>

static const int TTS_SAMPLE_RATE = 22050;
static const int TTS_CHUNK_FRAMES = 512;


Text2SpeechManager::Text2SpeechManager(const std::string& model_path,
                                       const std::string& alsa_sink)
    : model_(model_path), alsa_sink_(alsa_sink) {
    if (access(model_path.c_str(), R_OK) != 0)
        throw std::runtime_error(
            std::string("TTS model not found: ") + model_path + "\n"
            "Run:  ./scripts/download_models.sh");
}


void Text2SpeechManager::speak(const std::string& text) {
    // Pipe pair for parent→piper stdin and piper stdout→parent
    int to_piper[2], from_piper[2];
    if (pipe(to_piper) || pipe(from_piper)) {
        std::cerr << "[Velan] TTS pipe() failed\n";
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[Velan] TTS fork() failed\n";
        close(to_piper[0]); close(to_piper[1]);
        close(from_piper[0]); close(from_piper[1]);
        return;
    }

    if (pid == 0) {
        // Child: wire pipes to stdin/stdout, suppress piper's stderr, exec
        dup2(to_piper[0],   STDIN_FILENO);
        dup2(from_piper[1], STDOUT_FILENO);
        close(to_piper[0]); close(to_piper[1]);
        close(from_piper[0]); close(from_piper[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);

        execlp("piper", "piper", "--model", model_.c_str(), "--output_raw", nullptr);
        _exit(1);
    }

    // Parent: write text to piper, read back raw PCM
    close(to_piper[0]);
    close(from_piper[1]);

    const std::string line = text + "\n";
    write(to_piper[1], line.c_str(), line.size());
    close(to_piper[1]);

    std::vector<int16_t> samples;
    int16_t buf[TTS_CHUNK_FRAMES];
    ssize_t n;
    while ((n = read(from_piper[0], buf, sizeof(buf))) > 0)
        samples.insert(samples.end(), buf, buf + n / sizeof(int16_t));
    close(from_piper[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        std::cerr << "[Velan] piper exited with error — is piper installed?\n";
        return;
    }

    if (samples.empty()) return;

    // ---------------------------------------------------------------------------
    // Playback — two-tier strategy:
    //
    //  1. PortAudio (preferred on PC): try paInt16 first, then paFloat32.
    //     Skipped entirely when alsa_sink_ == "pulse" — on RPi the ALSA default
    //     PCM never exposes S16/F32 to PortAudio's hw: probe, so attempting it
    //     only produces 6 noisy ALSA error lines with no chance of success.
    //
    //  2. Command-line player fallback (always used on RPi --tts-sink pulse):
    //       "pulse"      → pacat --playback (PulseAudio/PipeWire, reads stdin)
    //       "<alsa-dev>" → aplay -D <dev>   (explicit ALSA hw device)
    //       ""           → aplay            (ALSA default)
    // ---------------------------------------------------------------------------

    PaStream* stream    = nullptr;
    bool      use_float = false;
    bool      pa_ok     = false;

    // Skip PortAudio when the caller has already chosen an explicit player sink.
    // For "pulse" the ALSA hw: probe always fails on RPi; trying it just spams
    // the log with ALSA errors before falling through to pacat anyway.
    const bool skip_portaudio = !alsa_sink_.empty();

    if (!skip_portaudio) {
        PaError err = Pa_OpenDefaultStream(&stream, 0, 1, paInt16,
                                           TTS_SAMPLE_RATE, TTS_CHUNK_FRAMES,
                                           nullptr, nullptr);
        if (err == paNoError) {
            pa_ok = true;
        } else {
            // paInt16 rejected — try paFloat32
            err = Pa_OpenDefaultStream(&stream, 0, 1, paFloat32,
                                       TTS_SAMPLE_RATE, TTS_CHUNK_FRAMES,
                                       nullptr, nullptr);
            if (err == paNoError) {
                pa_ok     = true;
                use_float = true;
            }
        }
    }

    if (pa_ok) {
        Pa_StartStream(stream);

        if (use_float) {
            std::vector<float> fbuf(TTS_CHUNK_FRAMES);
            for (size_t i = 0; i < samples.size(); i += TTS_CHUNK_FRAMES) {
                unsigned long count = std::min((size_t)TTS_CHUNK_FRAMES, samples.size() - i);
                for (size_t j = 0; j < count; ++j)
                    fbuf[j] = samples[i + j] / 32768.0f;
                Pa_WriteStream(stream, fbuf.data(), count);
            }
        } else {
            for (size_t i = 0; i < samples.size(); i += TTS_CHUNK_FRAMES) {
                unsigned long count = std::min((size_t)TTS_CHUNK_FRAMES, samples.size() - i);
                Pa_WriteStream(stream, samples.data() + i, count);
            }
        }

        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        return;
    }

    // PortAudio failed — choose a command-line player based on the sink:
    //
    //   alsa_sink_ == "pulse"  →  paplay  (PulseAudio/PipeWire native; no ALSA
    //                             plugin needed, works with BT speakers via PipeWire)
    //   alsa_sink_ == ""       →  aplay   (ALSA default device)
    //   alsa_sink_ == anything →  aplay -D <sink>  (explicit ALSA device)
    //
    // All three read raw signed 16-bit LE mono PCM from stdin.

    const bool use_paplay = (alsa_sink_ == "pulse");

    // Only log the fallback reason when PortAudio was actually attempted and
    // failed (not when it was intentionally skipped for a known-good sink).
    if (!skip_portaudio) {
        if (use_paplay)
            std::cerr << "[Velan] TTS: PortAudio unavailable — falling back to pacat (PulseAudio/PipeWire)\n";
        else {
            std::cerr << "[Velan] TTS: PortAudio unavailable — falling back to aplay";
            if (!alsa_sink_.empty()) std::cerr << " (device: " << alsa_sink_ << ")";
            std::cerr << "\n";
        }
    }

    int to_player[2];
    if (pipe(to_player) < 0) {
        std::cerr << "[Velan] TTS player pipe() failed\n";
        return;
    }

    pid_t ppid = fork();
    if (ppid < 0) {
        std::cerr << "[Velan] TTS player fork() failed\n";
        close(to_player[0]); close(to_player[1]);
        return;
    }

    if (ppid == 0) {
        dup2(to_player[0], STDIN_FILENO);
        close(to_player[0]); close(to_player[1]);

        std::string rate_str = std::to_string(TTS_SAMPLE_RATE);

        if (use_paplay) {
            // pacat --playback reads raw PCM from stdin (fd 0) with no filename
            // argument.  "paplay" is the same binary but expects a WAV file path;
            // passing "-" makes it try to open a literal file named "-" and fail
            // with "open(): No such file or directory".
            execlp("pacat", "pacat",
                   "--playback",
                   "--raw",
                   "--format=s16le",
                   (std::string("--rate=") + rate_str).c_str(),
                   "--channels=1",
                   nullptr);
        } else if (alsa_sink_.empty()) {
            execlp("aplay", "aplay",
                   "-r", rate_str.c_str(),
                   "-f", "S16_LE",
                   "-c", "1",
                   "-q", "-",
                   nullptr);
        } else {
            execlp("aplay", "aplay",
                   "-D", alsa_sink_.c_str(),
                   "-r", rate_str.c_str(),
                   "-f", "S16_LE",
                   "-c", "1",
                   "-q", "-",
                   nullptr);
        }
        _exit(1);
    }

    close(to_player[0]);

    // Guard against SIGPIPE: if the player exits early (device error), writing
    // to the broken pipe would send SIGPIPE and crash velan.  Suppress it for
    // the duration of the write and rely on the write() return value instead.
    struct sigaction sa_old {};
    struct sigaction sa_ign {};
    sa_ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa_ign, &sa_old);

    const char* ptr       = reinterpret_cast<const char*>(samples.data());
    size_t      remaining = samples.size() * sizeof(int16_t);
    while (remaining > 0) {
        ssize_t written = write(to_player[1], ptr, remaining);
        if (written <= 0) break;   // EPIPE or other error — player already gone
        ptr       += written;
        remaining -= static_cast<size_t>(written);
    }
    close(to_player[1]);

    sigaction(SIGPIPE, &sa_old, nullptr);   // restore previous SIGPIPE handler

    int pstatus = 0;
    waitpid(ppid, &pstatus, 0);
    if (WIFEXITED(pstatus) && WEXITSTATUS(pstatus) != 0) {
        if (use_paplay)
            std::cerr << "[Velan] TTS pacat failed — is pulseaudio-utils installed on the RPi?\n"
                         "        Run: sudo apt install pulseaudio-utils\n";
        else
            std::cerr << "[Velan] TTS aplay failed — check 'aplay -l' for available devices,\n"
                         "        then rerun with --tts-sink <device> (e.g. --tts-sink plughw:1,0)\n";
    }
}
