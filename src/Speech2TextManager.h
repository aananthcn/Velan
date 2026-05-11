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

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <whisper.h>

#include "TransformerManager.h"
#include "Text2SpeechManager.h"


class Speech2TextManager {
public:
    static Speech2TextManager& instance(const std::string& ollama_model,
                                        const char* stt_model_path,
                                        const char* tts_model_path);

    Speech2TextManager(const Speech2TextManager&)            = delete;
    Speech2TextManager& operator=(const Speech2TextManager&) = delete;

    void handle_trigger(int32_t state);
    void stop_if_recording();

private:
    Speech2TextManager(const std::string& ollama_model,
                       const char* stt_model_path,
                       const char* tts_model_path);
    ~Speech2TextManager();

    void process();

    TransformerManager     transformer_;
    Text2SpeechManager     tts_;
    whisper_context_params wparams_;
    whisper_context*       ctx_;

    bool               is_recording_;
    std::atomic<bool>  stop_recording_;
    std::thread        rec_thread_;
    std::vector<float> audio_;
};
