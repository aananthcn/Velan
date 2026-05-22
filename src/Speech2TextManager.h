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
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <whisper.h>


class Speech2TextManager {
public:
    // on_start:      called before recording begins — use it to pause WakeWordDetector.
    // on_transcript: called with the transcript after transcription completes (empty
    //                string on silence/error). Caller owns LLM, TTS, and the
    //                PROCESSING → LISTENING transition (wwd->resume()).
    static Speech2TextManager& instance(const char* stt_model_path,
                                        std::function<void()> on_start,
                                        std::function<void(const std::string&)> on_transcript,
                                        int mic_device = -1);

    Speech2TextManager(const Speech2TextManager&)            = delete;
    Speech2TextManager& operator=(const Speech2TextManager&) = delete;

    // Starts the PROCESSING pipeline: record → transcribe → LLM → TTS → on_done.
    // No-op if already recording.
    void handle_trigger();

    // Exposes the loaded Whisper context so WakeWordDetector can share it.
    whisper_context* get_context() const { return ctx_; }

private:
    Speech2TextManager(const char* stt_model_path,
                       std::function<void()> on_start,
                       std::function<void(const std::string&)> on_transcript,
                       int mic_device = -1);
    ~Speech2TextManager();

    std::string process();

    whisper_context_params                    wparams_;
    whisper_context*                          ctx_;
    std::function<void()>                     on_start_;
    std::function<void(const std::string&)>   on_transcript_;

    int                mic_device_;
    std::atomic<bool>  is_recording_;
    std::atomic<bool>  stop_recording_;
    std::thread        rec_thread_;
    std::vector<float> audio_;
};
