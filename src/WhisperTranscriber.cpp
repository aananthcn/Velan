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

#include "WhisperTranscriber.h"
#include "log.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>

static constexpr int SAMPLE_RATE  = 16000;
static constexpr int CHUNK_FRAMES = 1600;   // 100 ms @ 16 kHz

// Leave at least one core free for audio I/O and OS tasks.
static const int WHISPER_N_THREADS =
    std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);


WhisperTranscriber::WhisperTranscriber(const char* model_path)
    : ctx_(nullptr), n_threads_(WHISPER_N_THREADS) {
    wparams_ = whisper_context_default_params();
#ifdef GGML_USE_CUDA
    wparams_.use_gpu    = true;
    wparams_.gpu_device = 0;
    std::cout << log_ts() << "[Whisper] Loading model (GPU): " << model_path << "\n";
#else
    wparams_.use_gpu    = false;
    std::cout << log_ts() << "[Whisper] Loading model (CPU, " << n_threads_
              << " threads): " << model_path << "\n";
#endif
    ctx_ = whisper_init_from_file_with_params(model_path, wparams_);
    if (!ctx_)
        throw std::runtime_error(
            std::string("WhisperTranscriber: failed to load model: ") + model_path +
            "\n  Run:  ./scripts/download_models.sh");
    std::cout << log_ts() << "[Whisper] Model loaded.\n";
}


WhisperTranscriber::~WhisperTranscriber() {
    if (ctx_) whisper_free(ctx_);
}


std::string WhisperTranscriber::transcribe(const std::vector<float>& pcm,
                                            bool               single_segment,
                                            int                audio_ctx,
                                            const std::string& initial_prompt) {
    if (pcm.empty()) return {};

    // whisper_full_with_state rejects audio < 1000 ms due to mel-frame rounding
    // (16000 samples is reported as 990 ms internally). Pad to 1100 ms minimum.
    constexpr int MIN_SAMPLES = SAMPLE_RATE + CHUNK_FRAMES;  // 17600 = 1100 ms
    const std::vector<float>* src = &pcm;
    std::vector<float> padded;
    if ((int)pcm.size() < MIN_SAMPLES) {
        padded = pcm;
        padded.resize(MIN_SAMPLES, 0.0f);
        src = &padded;
    }

    whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads        = n_threads_;
    p.language         = "en";
    p.translate        = false;
    p.print_special    = false;
    p.print_progress   = false;
    p.print_realtime   = false;
    p.print_timestamps = false;
    p.single_segment   = single_segment;
    // initial_prompt primes the decoder with expected vocabulary.  Used in WWD
    // mode to anchor the beam search toward the wake-phrase words before decoding
    // starts.  This is the most effective fix for proper-noun hallucinations.
    if (!initial_prompt.empty())
        p.initial_prompt = initial_prompt.c_str();
    // no_context: when an initial_prompt is set it already provides context, so
    // we still disable cross-segment context accumulation (each window is independent).
    p.no_context       = true;
    if (audio_ctx > 0) {
        p.audio_ctx = audio_ctx;
        // WWD mode — optimise for speed, not quality.  We only need phrase
        // matching; the repetition-loop and noise-pattern filters handle
        // anything Whisper produces on a bad clip.
        //
        // 1. Lower no_speech_thold so near-silence emits [BLANK_AUDIO]
        //    without triggering any temperature fallback.
        p.no_speech_thold = 0.4f;   // default 0.6

        // 2. Disable temperature fallback retries entirely.
        //    Without initial_prompt (removed because it caused 100% repetition
        //    loops), the default thresholds (entropy_thold=2.4, logprob_thold=-1.0)
        //    are exceeded by most 1-second clips that contain trailing silence,
        //    firing 4–6 retries at ~500 ms each on CUDA → 2000–5000 ms total.
        //    Setting them to ±∞ means inference always runs exactly once.
        //    Note: LOOSEN (not tighten) these thresholds to suppress retries.
        p.entropy_thold  =  100.0f; // default 2.4  — never trigger entropy retry
        p.logprob_thold  = -100.0f; // default -1.0 — never trigger logprob retry
    }

    if (whisper_full(ctx_, p, src->data(), static_cast<int>(src->size())) != 0)
        return {};

    std::string out;
    int n = whisper_full_n_segments(ctx_);
    for (int i = 0; i < n; ++i) {
        const char* s = whisper_full_get_segment_text(ctx_, i);
        if (s) out += s;
    }
    return out;
}
