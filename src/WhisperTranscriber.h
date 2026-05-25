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

#include "Transcriber.h"
#include <whisper.h>

// whisper.cpp backend — runs on CPU (default) or CUDA GPU (GGML_USE_CUDA).
// Loads the model once at construction; the context is reused across calls.
class WhisperTranscriber : public ITranscriber {
public:
    explicit WhisperTranscriber(const char* model_path);
    ~WhisperTranscriber() override;

    WhisperTranscriber(const WhisperTranscriber&)            = delete;
    WhisperTranscriber& operator=(const WhisperTranscriber&) = delete;

    std::string transcribe(const std::vector<float>& pcm,
                           bool single_segment = false,
                           int  audio_ctx      = 0) override;

private:
    whisper_context*      ctx_;
    whisper_context_params wparams_;
    int                   n_threads_;
};
