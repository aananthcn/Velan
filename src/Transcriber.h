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

#pragma once

#include <string>
#include <vector>

// Speech-to-text backend abstraction.
//
// Implementations:
//   WhisperTranscriber  — whisper.cpp on CPU or CUDA GPU (default)
//   HailoTranscriber    — Hailo-8 NPU via HailoRT (Phase 2)
//
// Thread-safety: not thread-safe. The LISTENING/PROCESSING state machine
// ensures only one component (WWD or STT) calls transcribe() at a time.
class ITranscriber {
public:
    virtual ~ITranscriber() = default;

    // Transcribe PCM audio (16 kHz, mono, float32) to text.
    // Returns empty string on failure or when the audio contains no speech.
    //
    // single_segment:  true for short wake-word clips (forces one output segment);
    //                  false for user-utterance STT (natural multi-sentence output).
    // audio_ctx:       encoder context window in mel frames (0 = model default).
    //                  Set to 128 (~1.3 s) for wake-word to speed up CPU inference.
    // initial_prompt:  decoder context hint — text the model treats as preceding the
    //                  audio.  For wake-word mode pass the wake phrases (e.g.
    //                  "Subramanya Subramani Hey Vela") so the decoder is anchored to
    //                  the expected vocabulary before it starts.  This dramatically
    //                  reduces hallucinations on out-of-dictionary proper nouns.
    //                  Empty string = no hint (default for STT).
    virtual std::string transcribe(const std::vector<float>& pcm,
                                   bool               single_segment  = false,
                                   int                audio_ctx       = 0,
                                   const std::string& initial_prompt  = "") = 0;
};
