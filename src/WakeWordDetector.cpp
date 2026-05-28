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
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <portaudio.h>

// Whisper hallucinations on silence/noise. Filtered before phrase matching so
// ambient sound never accidentally triggers the wake word.
// Note: do NOT enumerate every repetition word here — that is whack-a-mole.
// The is_repetition_loop() check below catches all repetition-loop patterns
// generically regardless of which word Whisper chooses to loop.
static const char* kNoisePatterns[] = {
    "(", "[", "Thank you", "Thanks for watching"
};

// ---------------------------------------------------------------------------
// is_repetition_loop — detect Whisper's repetition-loop hallucination.
//
// When fed near-silence or a noise burst the model sometimes gets stuck
// repeating the same word ("Romania, Romania, Romania, ...") regardless of
// what was actually spoken.  Any word appearing >= 3 times in a 1-second
// window is almost certainly a hallucination — a real wake phrase never
// repeats the same word that many times.
// ---------------------------------------------------------------------------
static bool is_repetition_loop(const std::string& normalised) {
    std::unordered_map<std::string, int> freq;
    std::istringstream ss(normalised);
    std::string w;
    while (ss >> w) {
        if (++freq[w] >= 3) return true;
    }
    return false;
}

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
static constexpr float WWD_NOISE_MULTIPLIER = 1.2f;  // threshold = noise_floor × 1.2
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

    // Each entry in wake_phrases is either:
    //   "regex:<pattern>"  — compiled as ECMAScript case-insensitive std::regex
    //   plain text         — lower-cased, punctuation→spaces substring match
    for (const auto& phrase : wake_phrases) {
        if (phrase.rfind("regex:", 0) == 0) {
            // ── regex path ───────────────────────────────────────────────────
            std::string src = phrase.substr(6);   // strip the "regex:" prefix
            try {
                wake_patterns_.emplace_back(
                    src,
                    std::regex(src,
                               std::regex_constants::ECMAScript |
                               std::regex_constants::icase));
            } catch (const std::regex_error& e) {
                throw std::runtime_error(
                    std::string("[WakeWord] invalid regex '") + src + "': " + e.what());
            }
        } else {
            // ── plain-phrase path: normalise as before ───────────────────────
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
    }

    if (Pa_Initialize() != paNoError)
        throw std::runtime_error("[WakeWord] PortAudio init failed");

    std::cout << log_ts() << "[WakeWord] Wake phrase(s) / pattern(s):\n";
    for (const auto& p : wake_phrases_)
        std::cout << "  - substring: \"" << p << "\"\n";
    for (const auto& [src, re] : wake_patterns_)
        std::cout << "  - regex:     \"" << src << "\"\n";
}


WakeWordDetector::~WakeWordDetector() {
    stop();
    // After stop() the thread has seen running_=false and been detached.
    // If it is currently blocked in Pa_ReadStream it will return within one
    // CHUNK_MS (100 ms) and call close_stream().  If it is inside
    // whisper_full() the stream is already closed (detect_loop closes it
    // before every inference call), so Pa_Terminate races nothing.
    // 200 ms is > 2× CHUNK_MS — enough margin for the ReadStream path.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
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

    // Non-ASCII filter: all wake phrases are ASCII.  Any output with significant
    // non-ASCII content is a language-confusion hallucination (e.g. Chinese
    // characters produced by the multilingual model despite language="en").
    // Allow up to 3 non-ASCII bytes (one accented character) as a small tolerance.
    {
        int non_ascii = 0;
        for (unsigned char c : text) {
            if (c >= 0x80 && ++non_ascii > 3) {
                std::cout << log_ts() << "[WakeWord] Filtered (non-ASCII): \""
                          << text << "\"\n";
                return false;
            }
        }
    }

    // Reject known Whisper hallucinations before printing or matching.
    for (const char* pat : kNoisePatterns) {
        if (text.find(pat) != std::string::npos) {
            std::cout << log_ts() << "[WakeWord] Heard (filtered): \"" << text << "\"\n";
            return false;
        }
    }

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

    // Repetition-loop check must come before phrase matching so that a word
    // in the wake-phrase list (e.g. "subramanya subramanya subramanya ...") is
    // not accidentally matched.
    if (is_repetition_loop(normalised)) {
        std::cout << log_ts() << "[WakeWord] Filtered (repetition loop): \""
                  << text << "\"\n";
        return false;
    }

    std::cout << log_ts() << "[WakeWord] Heard: \"" << text << "\"\n";

    for (const auto& phrase : wake_phrases_) {
        if (normalised.find(phrase) != std::string::npos)
            return true;
    }
    // Regex patterns — applied to the same normalised (lower-case, punct-free) text.
    for (const auto& [src, re] : wake_patterns_) {
        if (std::regex_search(normalised, re))
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

    // Onset persistence: require 2 consecutive above-threshold chunks before
    // declaring a genuine onset.  A single 100 ms transient (keyboard click,
    // AC noise burst, door knock) will have onset_count == 1 and be discarded
    // when the next chunk falls below threshold.  Real speech onset (fricatives,
    // vowels) sustains for >= 200 ms and confirms on the second chunk.
    // The first candidate chunk is stored in pre_onset_chunk so it is prepended
    // to speech_buf — we never clip the word start.
    int                onset_count = 0;
    std::vector<float> pre_onset_chunk;
    pre_onset_chunk.reserve(CHUNK_FRAMES);

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
                in_speech   = false;
                onset_count = 0;
                speech_buf.clear();
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this] { return !stt_active_ || !running_.load(); });
                // TTS audio may still be reverberating in the room.  Wait 400 ms
                // for it to decay before reopening the mic and recalibrating the
                // noise floor — otherwise the inflated RMS drives eff_threshold up
                // and detection becomes progressively harder after each interaction.
                if (running_.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(400));
                std::cout << log_ts() << "[WakeWord] STT done — resuming detection.\n";
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
            in_speech   = false;
            onset_count = 0;
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
                if (onset_count == 0) {
                    // First above-threshold chunk: save it, wait for confirmation.
                    pre_onset_chunk.assign(chunk.begin(), chunk.end());
                    std::cout << log_ts() << "[WakeWord] Pre-onset  RMS=" << rms
                              << " thresh=" << eff_threshold << " — confirming...\n";
                }
                if (++onset_count >= 2) {
                    // Second consecutive chunk above threshold: genuine speech onset.
                    std::cout << log_ts() << "[WakeWord] Onset confirmed  RMS=" << rms
                              << " thresh=" << eff_threshold
                              << " — capturing " << LOCK_IN_MS << " ms...\n";
                    in_speech   = true;
                    onset_count = 0;
                    speech_buf.clear();
                    // Include the first confirming chunk so the word start is not clipped.
                    speech_buf.insert(speech_buf.end(),
                                      pre_onset_chunk.begin(), pre_onset_chunk.end());
                    speech_buf.insert(speech_buf.end(), chunk.begin(), chunk.end());
                }
            } else {
                if (onset_count > 0) {
                    std::cout << log_ts()
                              << "[WakeWord] Onset cancelled (transient) — RMS=" << rms
                              << " fell below thresh=" << eff_threshold << "\n";
                }
                onset_count = 0;
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

                // Trim trailing silence.
                //
                // The fixed 1000 ms window guarantees the full wake phrase was
                // captured, but the phrase typically ends at 400–600 ms; the
                // remaining 400–600 ms of ambient noise (RMS ≈ noise floor) is
                // the primary source of hallucinations ("So Brahmari", "speaking
                // in foreign language", etc.).  Trim chunk-by-chunk from the end
                // while RMS < 70 % of eff_threshold (below noise-floor level),
                // always keeping at least MIN_PHRASE_FRAMES.
                // WhisperTranscriber::transcribe() zero-pads any under-1100 ms
                // buffer internally; zeros produce [BLANK_AUDIO], not hallucinations.
                {
                    int trim_end = static_cast<int>(speech_buf.size());
                    while (trim_end - CHUNK_FRAMES >= MIN_PHRASE_FRAMES) {
                        float energy = 0.0f;
                        for (int k = trim_end - CHUNK_FRAMES; k < trim_end; ++k)
                            energy += speech_buf[k] * speech_buf[k];
                        if (std::sqrt(energy / CHUNK_FRAMES) >= eff_threshold * 0.7f)
                            break;
                        trim_end -= CHUNK_FRAMES;
                    }
                    if (trim_end < static_cast<int>(speech_buf.size())) {
                        int orig_ms = buf_ms;
                        buf_ms = trim_end * 1000 / SAMPLE_RATE;
                        std::cout << log_ts() << "[WakeWord] Trimmed "
                                  << (orig_ms - buf_ms) << " ms silence → "
                                  << buf_ms << " ms fed to Whisper.\n";
                        speech_buf.resize(trim_end);
                    }
                }

                std::cout << log_ts() << "[WakeWord] Transcribing " << buf_ms
                          << " ms (Whisper)...\n";
                auto t0 = std::chrono::steady_clock::now();
                // Do NOT pass wwd_prompt_ as initial_prompt here.
                // When the wake-phrase words appear in the prompt AND in the audio,
                // Whisper's decoder anchors to them and emits a repetition loop
                // ("Subrahmanya, Subrahmanya, Subrahmanya...") which is then caught
                // by is_repetition_loop() and filtered — producing 0% success rate.
                // The phrase list + existing filters are the right recognition strategy.
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
                    // Same 400 ms post-TTS reverb delay as the yield path above.
                    if (running_.load())
                        std::this_thread::sleep_for(std::chrono::milliseconds(400));
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
