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

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>


class Speech2TextManager {
public:
    // transcriber:   shared backend (WhisperTranscriber or HailoTranscriber).
    // on_start:      called before recording begins — use it to pause WakeWordDetector.
    // on_transcript: called with the transcript after transcription completes (empty
    //                string on silence/error). Caller owns LLM, TTS, and the
    //                PROCESSING → LISTENING transition (wwd->resume()).
    static Speech2TextManager& instance(ITranscriber* transcriber,
                                        std::function<void()> on_start,
                                        std::function<void(const std::string&)> on_transcript,
                                        int mic_device = -1);

    Speech2TextManager(const Speech2TextManager&)            = delete;
    Speech2TextManager& operator=(const Speech2TextManager&) = delete;

    // Starts the PROCESSING pipeline: record → transcribe → on_transcript.
    // No-op if already recording.
    void handle_trigger();

private:
    Speech2TextManager(ITranscriber* transcriber,
                       std::function<void()> on_start,
                       std::function<void(const std::string&)> on_transcript,
                       int mic_device);
    ~Speech2TextManager();

    std::string process();

    ITranscriber*                             transcriber_;   // non-owning
    std::function<void()>                     on_start_;
    std::function<void(const std::string&)>   on_transcript_;

    int                mic_device_;
    std::atomic<bool>  is_recording_;
    std::atomic<bool>  stop_recording_;
    std::thread        rec_thread_;
    std::vector<float> audio_;
};
