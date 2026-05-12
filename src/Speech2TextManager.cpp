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
#include <cmath>
#include <iostream>
#include <stdexcept>

#include <portaudio.h>

static const int   SAMPLE_RATE        = 16000;
static const int   CHUNK_FRAMES       = 1024;
static const int   WHISPER_THREADS    = 4;

// VAD parameters for end-of-speech detection inside record_audio().
static constexpr float VAD_RMS_THRESHOLD     = 0.01f;
static constexpr int   VAD_SILENCE_CHUNKS    = 15;              // ~960 ms of silence ends recording
static constexpr int   VAD_MIN_SPEECH_CHUNKS = 5;               // must see ~320 ms of speech first
static constexpr int   VAD_MAX_RECORD_FRAMES = SAMPLE_RATE * 90; // hard cap: 1.5 min

extern volatile bool g_interrupted;


// ---------------------------------------------------------------------------
// Audio recording — self-terminating via VAD.
// Stops when: sustained silence after speech, stop_flag set, or g_interrupted.
// ---------------------------------------------------------------------------
static std::vector<float> record_audio(std::atomic<bool>& stop_flag) {
    PaStream* stream = nullptr;
    PaError   err    = paNoError;

    // WWD may still be releasing the mic (up to one CHUNK_MS ≈ 100 ms).
    // Retry for up to 500 ms before giving up.
    for (int attempt = 0; attempt < 10 && !stop_flag.load(); ++attempt) {
        err = Pa_OpenDefaultStream(&stream, 1, 0, paFloat32,
                                   SAMPLE_RATE, CHUNK_FRAMES, nullptr, nullptr);
        if (err == paNoError) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
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
    int  speech_chunks  = 0;
    int  silence_chunks = 0;
    bool speech_started = false;

    while (!g_interrupted && !stop_flag.load()) {
        if ((int)samples.size() >= VAD_MAX_RECORD_FRAMES) {
            std::cout << "[Velan] Max recording duration reached — stopping.\n";
            break;
        }

        if (Pa_ReadStream(stream, buf.data(), CHUNK_FRAMES) != paNoError) break;
        samples.insert(samples.end(), buf.begin(), buf.end());

        float energy = 0.0f;
        for (float s : buf) energy += s * s;
        const bool is_speech = std::sqrt(energy / CHUNK_FRAMES) >= VAD_RMS_THRESHOLD;

        if (is_speech) {
            ++speech_chunks;
            silence_chunks  = 0;
            speech_started  = (speech_chunks >= VAD_MIN_SPEECH_CHUNKS);
        } else if (speech_started) {
            if (++silence_chunks >= VAD_SILENCE_CHUNKS) {
                std::cout << "[Velan] End of speech detected.\n";
                break;
            }
        }
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
                                                  const char* tts_model_path,
                                                  std::function<void()> on_start,
                                                  std::function<void()> on_done) {
    static Speech2TextManager inst(ollama_model, stt_model_path, tts_model_path,
                                   std::move(on_start), std::move(on_done));
    return inst;
}

Speech2TextManager::Speech2TextManager(const std::string& ollama_model,
                                        const char* stt_model_path,
                                        const char* tts_model_path,
                                        std::function<void()> on_start,
                                        std::function<void()> on_done)
    : transformer_(ollama_model), tts_(tts_model_path), ctx_(nullptr),
      on_start_(std::move(on_start)), on_done_(std::move(on_done)),
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
    stop_recording_ = true;
    if (rec_thread_.joinable()) rec_thread_.join();
    whisper_free(ctx_);
    Pa_Terminate();
}


void Speech2TextManager::handle_trigger() {
    if (is_recording_.exchange(true)) return;   // already in PROCESSING state

    if (on_start_) on_start_();   // pause WWD before taking the mic

    if (rec_thread_.joinable()) rec_thread_.join();
    audio_.clear();
    stop_recording_ = false;

    rec_thread_ = std::thread([this]() {
        std::cout << "[Velan] PROCESSING — recording started.\n";
        try {
            audio_ = record_audio(stop_recording_);
        } catch (const std::exception& e) {
            std::cerr << "[Velan] Recording error: " << e.what() << "\n";
        }
        process();
        is_recording_ = false;
        if (on_done_) on_done_();   // PROCESSING → LISTENING: resume WakeWordDetector
    });
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
