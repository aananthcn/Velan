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

#include "WakeWordDetector.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include <portaudio.h>

static constexpr int SAMPLE_RATE = 16000;

// ---------------------------------------------------------------------------
// Wake word detection — VAD-gated utterance capture
// ---------------------------------------------------------------------------
// Audio is polled in CHUNK_MS increments.  When RMS exceeds vad_threshold_,
// chunks are accumulated into a speech buffer.  After END_SILENCE_MS of
// continuous silence the utterance is considered complete and handed to
// Whisper for transcription.  This avoids running inference on silence and
// prevents the sliding-window hallucination problem.
static constexpr int CHUNK_MS           = 100;
static constexpr int CHUNK_FRAMES       = SAMPLE_RATE * CHUNK_MS   / 1000; // 1600
static constexpr int END_SILENCE_MS     = 500;
static constexpr int END_SILENCE_CHUNKS = END_SILENCE_MS / CHUNK_MS;       // 5
static constexpr int MIN_PHRASE_MS      = 200;
static constexpr int MIN_PHRASE_FRAMES  = SAMPLE_RATE * MIN_PHRASE_MS / 1000; // 3200
static constexpr int MAX_BUFFER_MS      = 5000;
static constexpr int MAX_BUFFER_FRAMES  = SAMPLE_RATE * MAX_BUFFER_MS / 1000; // 80 000

// Fewer Whisper threads than the STT path; wake word detection is lightweight.
static constexpr int WWD_THREADS = 2;


// ---------------------------------------------------------------------------
// Transcribe PCM audio via whisper.cpp.
// Returns empty string on failure (treated as no match).
// ---------------------------------------------------------------------------
static std::string transcribe_window(whisper_context* ctx,
                                     const std::vector<float>& pcm) {
    whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads        = WWD_THREADS;
    p.language         = "en";
    p.translate        = false;
    p.print_special    = false;
    p.print_progress   = false;
    p.print_realtime   = false;
    p.print_timestamps = false;
    p.single_segment   = true;

    if (whisper_full(ctx, p, pcm.data(), static_cast<int>(pcm.size())) != 0)
        return {};

    std::string out;
    int n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; ++i) {
        const char* s = whisper_full_get_segment_text(ctx, i);
        if (s) out += s;
    }
    return out;
}


// ---------------------------------------------------------------------------
// WakeWordDetector
// ---------------------------------------------------------------------------
WakeWordDetector::WakeWordDetector(TriggerCallback                  cb,
                                   whisper_context*                 ctx,
                                   const std::vector<std::string>&  wake_phrases,
                                   float                            vad_threshold)
    : callback_(std::move(cb)),
      vad_threshold_(vad_threshold),
      running_(false),
      ctx_(ctx),
      stt_active_(false) {
    if (!ctx_)
        throw std::runtime_error("[WakeWord] null whisper_context — STT must be initialised first");
    if (wake_phrases.empty())
        throw std::runtime_error("[WakeWord] at least one wake phrase is required");

    // Normalise each phrase: lower-case, punctuation → spaces, collapse runs.
    for (const auto& phrase : wake_phrases) {
        std::string norm;
        bool prev_space = true;
        for (unsigned char c : phrase) {
            if (std::isalpha(c)) {
                norm += static_cast<char>(std::tolower(c));
                prev_space = false;
            } else if (!prev_space) {
                norm += ' ';
                prev_space = true;
            }
        }
        if (!norm.empty() && norm.back() == ' ') norm.pop_back();
        if (!norm.empty()) wake_phrases_.push_back(std::move(norm));
    }

    if (Pa_Initialize() != paNoError)
        throw std::runtime_error("[WakeWord] PortAudio init failed");

    std::cout << "[WakeWord] Sharing Whisper context with STT.\n";
    std::cout << "[WakeWord] Wake phrase(s):\n";
    for (const auto& p : wake_phrases_)
        std::cout << "  - \"" << p << "\"\n";
}


WakeWordDetector::~WakeWordDetector() {
    stop();
    // ctx_ is owned by Speech2TextManager — do not free it here.
    Pa_Terminate();
}


void WakeWordDetector::start() {
    if (running_.load()) return;
    running_ = true;
    thread_  = std::thread(&WakeWordDetector::detect_loop, this);
}


void WakeWordDetector::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();   // unblock detect_loop if it is waiting for STT
    if (thread_.joinable()) thread_.join();
}


void WakeWordDetector::pause() {
    std::lock_guard<std::mutex> lk(mutex_);
    stt_active_ = true;
    // detect_loop will see this at the top of its next iteration and yield the mic.
}


void WakeWordDetector::resume() {
    std::lock_guard<std::mutex> lk(mutex_);
    stt_active_ = false;
    cv_.notify_one();
}


// ---------------------------------------------------------------------------
// phrase_matches — case-insensitive, punctuation-tolerant substring match.
// Filters out Whisper's [BLANK_AUDIO] hallucination token.
// ---------------------------------------------------------------------------
bool WakeWordDetector::phrase_matches(const std::string& text) const {
    if (text.empty()) return false;
    if (text.find("[BLANK_AUDIO]") != std::string::npos) return false;

    std::cout << "[WakeWord] Heard: \"" << text << "\"\n";

    std::string normalised;
    normalised.reserve(text.size());
    bool prev_space = true;
    for (unsigned char c : text) {
        if (std::isalpha(c)) {
            normalised += static_cast<char>(std::tolower(c));
            prev_space = false;
        } else if (!prev_space) {
            normalised += ' ';
            prev_space = true;
        }
    }
    if (!normalised.empty() && normalised.back() == ' ')
        normalised.pop_back();

    for (const auto& phrase : wake_phrases_) {
        if (normalised.find(phrase) != std::string::npos)
            return true;
    }
    return false;
}


// ---------------------------------------------------------------------------
// detect_loop — VAD-gated utterance capture
//
// Phase 1 — speech accumulation:
//   Polls mic in CHUNK_MS increments.  Chunks above vad_threshold_ are
//   appended to speech_buf.  After END_SILENCE_MS of trailing silence the
//   utterance is considered complete.
//
// Phase 2 — transcription:
//   Only reached when a full utterance is ready (avoids inference on silence).
//   If the transcript contains the wake phrase, hands off to Speech2TextManager.
// ---------------------------------------------------------------------------
void WakeWordDetector::detect_loop() {
    std::vector<float> chunk(CHUNK_FRAMES);
    std::vector<float> speech_buf;
    speech_buf.reserve(MAX_BUFFER_FRAMES);
    int silence_chunks = 0;

    PaStream* stream = nullptr;

    auto open_stream = [&]() -> bool {
        PaError err = Pa_OpenDefaultStream(&stream, 1, 0, paFloat32,
                                           SAMPLE_RATE, CHUNK_FRAMES,
                                           nullptr, nullptr);
        if (err != paNoError) { stream = nullptr; return false; }
        err = Pa_StartStream(stream);
        if (err != paNoError) { Pa_CloseStream(stream); stream = nullptr; return false; }
        return true;
    };

    auto close_stream = [&]() {
        if (!stream) return;
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        stream = nullptr;
    };

    while (running_.load()) {
        // Yield the mic if STT is active (wake-word path or external VHAL trigger).
        {
            bool active;
            { std::lock_guard<std::mutex> lk(mutex_); active = stt_active_; }
            if (active) {
                close_stream();
                speech_buf.clear();
                silence_chunks = 0;
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this] { return !stt_active_ || !running_.load(); });
                std::cout << "[WakeWord] STT done — resuming detection.\n";
                continue;
            }
        }

        if (!stream && !open_stream()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        if (Pa_ReadStream(stream, chunk.data(), CHUNK_FRAMES) != paNoError) {
            close_stream();
            continue;
        }

        float energy = 0.0f;
        for (float s : chunk) energy += s * s;
        const bool is_speech = std::sqrt(energy / CHUNK_FRAMES) >= vad_threshold_;

        if (is_speech) {
            speech_buf.insert(speech_buf.end(), chunk.begin(), chunk.end());
            silence_chunks = 0;
        } else if (!speech_buf.empty()) {
            // Trailing silence — keep accumulating until END_SILENCE_CHUNKS
            speech_buf.insert(speech_buf.end(), chunk.begin(), chunk.end());
            ++silence_chunks;
        }

        // Decide whether to transcribe: utterance ended or buffer safety cap hit
        const bool utterance_ended = !speech_buf.empty() &&
                                     silence_chunks >= END_SILENCE_CHUNKS;
        const bool buffer_capped   = (int)speech_buf.size() >= MAX_BUFFER_FRAMES;

        if (utterance_ended || buffer_capped) {
            if ((int)speech_buf.size() >= MIN_PHRASE_FRAMES) {
                std::string text = transcribe_window(ctx_, speech_buf);
                if (phrase_matches(text)) {
                    close_stream();
                    std::cout << "[WakeWord] Phrase matched — handing off to STT.\n";
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        stt_active_ = true;
                    }
                    callback_();
                    // LISTENING → PROCESSING: block until STT pipeline finishes
                    {
                        std::unique_lock<std::mutex> lk(mutex_);
                        cv_.wait(lk, [this] { return !stt_active_ || !running_.load(); });
                    }
                    std::cout << "[WakeWord] STT done — resuming detection.\n";
                }
            }
            speech_buf.clear();
            silence_chunks = 0;
        }
    }

    close_stream();
}
