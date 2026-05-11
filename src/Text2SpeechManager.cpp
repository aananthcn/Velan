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
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <portaudio.h>

static const int TTS_SAMPLE_RATE = 22050;
static const int TTS_CHUNK_FRAMES = 512;


Text2SpeechManager::Text2SpeechManager(const std::string& model_path)
    : model_(model_path) {
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

    // Play raw PCM via PortAudio
    PaStream* stream = nullptr;
    PaError err = Pa_OpenDefaultStream(&stream, 0, 1, paInt16,
                                       TTS_SAMPLE_RATE, TTS_CHUNK_FRAMES,
                                       nullptr, nullptr);
    if (err != paNoError) {
        std::cerr << "[Velan] TTS Pa_OpenDefaultStream: " << Pa_GetErrorText(err) << "\n";
        return;
    }

    Pa_StartStream(stream);

    for (size_t i = 0; i < samples.size(); i += TTS_CHUNK_FRAMES) {
        unsigned long count = std::min((size_t)TTS_CHUNK_FRAMES, samples.size() - i);
        Pa_WriteStream(stream, samples.data() + i, count);
    }

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
}
