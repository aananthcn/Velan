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
#include "log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <portaudio.h>

// Whisper hallucinations on silence/noise. Filtered before phrase matching so
// ambient sound never accidentally triggers the wake word.
static const char* kNoisePatterns[] = {
    "(", "[", "Thank you", "Thanks for watching", "you"
};

static constexpr int SAMPLE_RATE = 16000;

// ---------------------------------------------------------------------------
// Wake word detection — fixed-duration capture after speech onset
// ---------------------------------------------------------------------------
// Audio is polled in CHUNK_MS increments.  When RMS exceeds eff_threshold,
// ALL subsequent chunks are accumulated into speech_buf for LOCK_IN_MS,
// regardless of per-chunk energy.  This avoids discarding quiet mid-word
// phonemes (e.g. vowels in "Subramanya") that fall below the VAD threshold.
// After LOCK_IN_MS the buffer is sent to Whisper for phrase matching.
static constexpr int CHUNK_MS           = 100;
static constexpr int CHUNK_FRAMES       = SAMPLE_RATE * CHUNK_MS   / 1000; // 1600 samples
static constexpr int MIN_PHRASE_MS      = 200;
static constexpr int MIN_PHRASE_FRAMES  = SAMPLE_RATE * MIN_PHRASE_MS / 1000; // 3200
// Fixed capture window after onset.  Must cover the longest expected wake phrase
// at a relaxed speaking pace.  "Subrahmanya" ≈ 700 ms; 1000 ms gives headroom.
static constexpr int LOCK_IN_MS         = 1000;
static constexpr int LOCK_IN_FRAMES     = SAMPLE_RATE * LOCK_IN_MS / 1000; // 16000

// Adaptive VAD: measure ambient RMS for this many chunks on each fresh stream open,
// then set threshold = noise_floor * multiplier, clamped to [vad_threshold_, MAX].
static constexpr int   WWD_NOISE_CAL_CHUNKS = 5;     // ~500 ms — short to minimise dead time
static constexpr float WWD_NOISE_MULTIPLIER = 1.5f;  // threshold = noise_floor × 1.5
static constexpr float WWD_THRESHOLD_MAX    = 0.10f;


// ---------------------------------------------------------------------------
// WakeWordDetector
// ---------------------------------------------------------------------------
WakeWordDetector::WakeWordDetector(TriggerCallback                  cb,
                                   ITranscriber*                    transcriber,
                                   const std::vector<std::string>&  wake_phrases,
                                   float                            vad_threshold,
                                   int                              mic_device)
    : callback_(std::move(cb)),
      transcriber_(transcriber),
      vad_threshold_(vad_threshold),
      mic_device_(mic_device),
      running_(false),
      stt_active_(false) {
    if (!transcriber_)
        throw std::runtime_error("[WakeWord] null transcriber — must be initialised first");
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

    std::cout << log_ts() << "[WakeWord] Wake phrase(s):" << std::endl;
    for (const auto& p : wake_phrases_)
        std::cout << "  - \"" << p << "\"" << std::endl;
}


WakeWordDetector::~WakeWordDetector() {
    stop();
    // transcriber_ is owned by main() — do not free it here.
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
    // Detach rather than join — whisper_full() can block for 30s+ on CPU.
    // The OS will clean up when the process exits.
    if (thread_.joinable()) thread_.detach();
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

    // Reject known Whisper hallucinations before printing or matching.
    for (const char* pat : kNoisePatterns) {
        if (text.find(pat) != std::string::npos) {
            std::cout << log_ts() << "[WakeWord] Heard (filtered): \"" << text << "\"\n";
            return false;
        }
    }

    std::cout << log_ts() << "[WakeWord] Heard: \"" << text << "\"\n";

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
// detect_loop — onset-triggered fixed-duration capture
//
// Calibration: measure ambient RMS for WWD_NOISE_CAL_CHUNKS, use median
// (not mean) to reject transient spikes during startup.
//
// Detection: wait for any chunk >= eff_threshold, then collect ALL chunks
// for LOCK_IN_MS regardless of per-chunk energy.  No VAD-based end detection
// inside the word — the whole window is sent to Whisper at once.
// ---------------------------------------------------------------------------
void WakeWordDetector::detect_loop() {
    std::vector<float> chunk(CHUNK_FRAMES);
    std::vector<float> speech_buf;
    speech_buf.reserve(LOCK_IN_FRAMES + CHUNK_FRAMES);
    std::vector<float> cal_rms;
    cal_rms.reserve(WWD_NOISE_CAL_CHUNKS);
    bool  cal_done      = false;
    float eff_threshold = vad_threshold_;
    bool  in_speech     = false;

    PaStream* stream = nullptr;

    auto open_stream = [&]() -> bool {
        PaDeviceIndex dev = (mic_device_ >= 0)
                            ? static_cast<PaDeviceIndex>(mic_device_)
                            : Pa_GetDefaultInputDevice();
        if (dev == paNoDevice) {
            std::cerr << log_ts() << "[WakeWord] No input device available.\n";
            return false;
        }
        const PaDeviceInfo* info = Pa_GetDeviceInfo(dev);

        PaStreamParameters p{};
        p.device                    = dev;
        p.channelCount              = 1;
        p.sampleFormat              = paFloat32;
        p.suggestedLatency          = info ? info->defaultLowInputLatency : 0.1;
        p.hostApiSpecificStreamInfo = nullptr;

        PaError err = Pa_OpenStream(&stream, &p, nullptr, SAMPLE_RATE,
                                    CHUNK_FRAMES, paClipOff, nullptr, nullptr);
        if (err != paNoError) {
            std::cerr << log_ts() << "[WakeWord] Pa_OpenStream failed (device " << dev
                      << "): " << Pa_GetErrorText(err) << "\n";
            stream = nullptr;
            return false;
        }
        err = Pa_StartStream(stream);
        if (err != paNoError) {
            std::cerr << log_ts() << "[WakeWord] Pa_StartStream failed: "
                      << Pa_GetErrorText(err) << "\n";
            Pa_CloseStream(stream);
            stream = nullptr;
            return false;
        }
        if (!cal_done) {
            std::cout << log_ts() << "[WakeWord] Microphone opened"
                      << (info ? std::string(": ") + info->name : "")
                      << " — calibrating VAD...\n";
            cal_rms.clear();
        } else {
            std::cout << log_ts() << "[WakeWord] Microphone opened"
                      << (info ? std::string(": ") + info->name : "")
                      << " — VAD threshold: " << eff_threshold
                      << " (reusing calibration).\n";
        }
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
                in_speech = false;
                speech_buf.clear();
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this] { return !stt_active_ || !running_.load(); });
                std::cout << log_ts() << "[WakeWord] STT done — resuming detection.\n";
                // TTS may have altered ambient level — recalibrate.
                cal_done = false;
                cal_rms.clear();
                continue;
            }
        }

        if (!stream && !open_stream()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        if (Pa_ReadStream(stream, chunk.data(), CHUNK_FRAMES) != paNoError) {
            std::cerr << log_ts() << "[WakeWord] Pa_ReadStream error — reopening stream.\n";
            close_stream();
            in_speech = false;
            speech_buf.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        float energy = 0.0f;
        for (float s : chunk) energy += s * s;
        float rms = std::sqrt(energy / CHUNK_FRAMES);

        // Calibration: collect WWD_NOISE_CAL_CHUNKS samples, then use their median
        // as the noise floor.  Median rejects transient spikes (keyboard, AC startup)
        // that inflate a mean-based estimate and drive eff_threshold too high.
        if (!cal_done) {
            cal_rms.push_back(rms);
            std::cout << log_ts() << "[WakeWord] CAL " << cal_rms.size()
                      << "/" << WWD_NOISE_CAL_CHUNKS << "  RMS=" << rms << "\n";
            if ((int)cal_rms.size() >= WWD_NOISE_CAL_CHUNKS) {
                std::vector<float> sorted = cal_rms;
                std::sort(sorted.begin(), sorted.end());
                float noise_floor = sorted[sorted.size() / 2];  // median
                eff_threshold = std::min(WWD_THRESHOLD_MAX,
                                std::max(vad_threshold_, noise_floor * WWD_NOISE_MULTIPLIER));
                std::cout << log_ts() << "[WakeWord] Noise floor (median): " << noise_floor
                          << "  VAD threshold: " << eff_threshold
                          << " — listening for wake phrase.\n";
                cal_done = true;
            }
            continue;
        }

        if (!in_speech) {
            if (rms >= eff_threshold) {
                std::cout << log_ts() << "[WakeWord] Onset  RMS=" << rms
                          << " thresh=" << eff_threshold
                          << " — capturing " << LOCK_IN_MS << " ms...\n";
                in_speech = true;
                speech_buf.clear();
                speech_buf.insert(speech_buf.end(), chunk.begin(), chunk.end());
            }
        } else {
            // Fixed-duration capture: accumulate ALL chunks regardless of energy.
            // Quiet mid-word phonemes stay in the buffer; Whisper handles them.
            speech_buf.insert(speech_buf.end(), chunk.begin(), chunk.end());
            int buf_ms = static_cast<int>(speech_buf.size() * 1000 / SAMPLE_RATE);
            std::cout << log_ts() << "[WakeWord] Capture "
                      << buf_ms << "/" << LOCK_IN_MS << " ms  RMS=" << rms << "\n";

            if ((int)speech_buf.size() >= LOCK_IN_FRAMES) {
                in_speech = false;
                // Close mic before inference — Whisper can block for seconds on CPU.
                close_stream();
                std::cout << log_ts() << "[WakeWord] Transcribing " << buf_ms
                          << " ms (Whisper)...\n";
                auto t0 = std::chrono::steady_clock::now();
                std::string text = transcriber_->transcribe(speech_buf,
                                                             /*single_segment=*/true,
                                                             /*audio_ctx=*/128);
                auto inf_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - t0).count();
                std::cout << log_ts() << "[WakeWord] Whisper done in " << inf_ms << " ms.\n";

                if (phrase_matches(text)) {
                    std::cout << log_ts() << "[WakeWord] Phrase matched — handing off to STT.\n";
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        stt_active_ = true;
                    }
                    callback_();
                    // Block until STT pipeline finishes (PROCESSING → LISTENING).
                    {
                        std::unique_lock<std::mutex> lk(mutex_);
                        cv_.wait(lk, [this] { return !stt_active_ || !running_.load(); });
                    }
                    std::cout << log_ts() << "[WakeWord] STT done — resuming detection.\n";
                    cal_done = false;
                    cal_rms.clear();
                } else {
                    // Cooldown: give the user a clear window to speak before listening
                    // again. Without this, GPU inference completes in ~100 ms and the
                    // next false trigger fires immediately, blocking the user's voice.
                    std::cout << log_ts() << "[WakeWord] No match — cooldown 1 s.\n";
                    for (int i = 0; i < 10 && running_.load(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                speech_buf.clear();
            }
        }
    }

    close_stream();
}
