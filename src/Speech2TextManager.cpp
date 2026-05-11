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

#include "Speech2TextManager.h"

#include <array>
#include <iostream>
#include <stdexcept>

#include <portaudio.h>

static const int SAMPLE_RATE     = 16000;
static const int CHUNK_FRAMES    = 1024;
static const int WHISPER_THREADS = 4;

extern volatile bool g_interrupted;


// ---------------------------------------------------------------------------
// Audio recording — stops when stop_flag is set or g_interrupted fires
// ---------------------------------------------------------------------------
static std::vector<float> record_audio(std::atomic<bool>& stop_flag) {
    PaStream* stream = nullptr;
    PaError   err;

    err = Pa_OpenDefaultStream(&stream, 1, 0, paFloat32,
                               SAMPLE_RATE, CHUNK_FRAMES, nullptr, nullptr);
    if (err != paNoError)
        throw std::runtime_error(std::string("Pa_OpenDefaultStream: ") + Pa_GetErrorText(err));

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        Pa_CloseStream(stream);
        throw std::runtime_error(std::string("Pa_StartStream: ") + Pa_GetErrorText(err));
    }

    std::vector<float> samples;
    samples.reserve(SAMPLE_RATE * 30);

    std::array<float, CHUNK_FRAMES> buf;
    while (!g_interrupted && !stop_flag.load()) {
        Pa_ReadStream(stream, buf.data(), CHUNK_FRAMES);
        samples.insert(samples.end(), buf.begin(), buf.end());
    }

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    return samples;
}


// ---------------------------------------------------------------------------
// Whisper transcription
// ---------------------------------------------------------------------------
static std::string transcribe(whisper_context* ctx, const std::vector<float>& pcm) {
    if (pcm.empty()) return {};

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads           = WHISPER_THREADS;
    params.language            = "en";
    params.translate           = false;
    params.print_special       = false;
    params.print_progress      = false;
    params.print_realtime      = false;
    params.print_timestamps    = false;
    params.single_segment      = false;

    if (whisper_full(ctx, params, pcm.data(), static_cast<int>(pcm.size())) != 0)
        throw std::runtime_error("whisper_full() failed");

    std::string text;
    int n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; ++i) {
        const char* seg = whisper_full_get_segment_text(ctx, i);
        if (seg) text += seg;
    }
    return text;
}


// ---------------------------------------------------------------------------
// Speech2TextManager
// ---------------------------------------------------------------------------
Speech2TextManager& Speech2TextManager::instance(const std::string& ollama_model,
                                                  const char* stt_model_path,
                                                  const char* tts_model_path) {
    static Speech2TextManager inst(ollama_model, stt_model_path, tts_model_path);
    return inst;
}

Speech2TextManager::Speech2TextManager(const std::string& ollama_model,
                                        const char* stt_model_path,
                                        const char* tts_model_path)
    : transformer_(ollama_model), tts_(tts_model_path), ctx_(nullptr),
      is_recording_(false), stop_recording_(false) {
    if (Pa_Initialize() != paNoError)
        throw std::runtime_error("PortAudio init failed");

    wparams_ = whisper_context_default_params();
#ifdef GGML_USE_CUDA
    wparams_.use_gpu    = true;
    wparams_.gpu_device = 0;
    std::cout << "[Velan] Loading Whisper model (GPU): " << stt_model_path << "\n";
#else
    wparams_.use_gpu    = false;
    std::cout << "[Velan] Loading Whisper model (CPU): " << stt_model_path << "\n";
#endif
    ctx_ = whisper_init_from_file_with_params(stt_model_path, wparams_);
    if (!ctx_) {
        Pa_Terminate();
        throw std::runtime_error(
            std::string("Failed to load Whisper model from: ") + stt_model_path + "\n"
            "Run:  ./scripts/download_models.sh");
    }

    std::cout << "[Velan] Whisper model loaded.\n";
}

Speech2TextManager::~Speech2TextManager() {
    stop_if_recording();
    whisper_free(ctx_);
    Pa_Terminate();
}


void Speech2TextManager::handle_trigger(int32_t state) {
    if (state == 1 && !is_recording_) {
        audio_.clear();
        stop_recording_ = false;
        is_recording_   = true;
        rec_thread_ = std::thread([this]() {
            try {
                audio_ = record_audio(stop_recording_);
            } catch (const std::exception& e) {
                std::cerr << "[Velan] Recording error: " << e.what() << "\n";
            }
        });
        std::cout << "[Velan] TRIGGER_ON  — recording started.\n";

    } else if (state == 0 && is_recording_) {
        stop_recording_ = true;
        is_recording_   = false;
        if (rec_thread_.joinable()) rec_thread_.join();
        std::cout << "[Velan] TRIGGER_OFF — recording stopped.\n";
        process();
    }
}


void Speech2TextManager::stop_if_recording() {
    if (is_recording_) {
        stop_recording_ = true;
        if (rec_thread_.joinable()) rec_thread_.join();
        is_recording_ = false;
    }
}


void Speech2TextManager::process() {
    if (audio_.size() < static_cast<size_t>(SAMPLE_RATE / 2)) {
        std::cout << "[Velan] Audio too short — skipping.\n";
        return;
    }

    std::cout << "[Velan] Transcribing...\n";
    std::string transcript;
    try {
        transcript = transcribe(ctx_, audio_);
    } catch (const std::exception& e) {
        std::cerr << "[Velan] Transcription error: " << e.what() << "\n";
        return;
    }

    if (transcript.empty()) {
        std::cout << "[Velan] (nothing transcribed)\n";
        return;
    }
    std::cout << "[Velan] You said: " << transcript << "\n";

    std::cout << "[Velan] Thinking...\n";
    try {
        std::string reply = transformer_.chat(transcript);
        std::cout << "[Velan] Assistant: " << reply << "\n\n";
        tts_.speak(reply);
    } catch (const std::exception& e) {
        std::cerr << "[Velan] Ollama error: " << e.what() << "\n";
    }
}
