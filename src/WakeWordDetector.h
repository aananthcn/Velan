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
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <whisper.h>

#define WWD_DEFAULT_WAKE_WORD "Hey Vela, Subramanya, Subramani, Subrahmanya, Supramani"

// Continuously listens on the microphone in LISTENING state.
// When the wake phrase is detected, fires callback() and blocks until
// resume() is called — signalling that STT/Chat/TTS have finished.
// The two states (LISTENING / PROCESSING) are mutually exclusive: only
// one component owns the microphone at a time.
//
// Shares the whisper_context* owned by Speech2TextManager — no second model
// is loaded. Safe because LISTENING and PROCESSING never overlap.
class WakeWordDetector {
public:
    using TriggerCallback = std::function<void()>;

    // wake_phrases: one or more phrases; any match fires the trigger.
    WakeWordDetector(TriggerCallback                  cb,
                     whisper_context*                 ctx,
                     const std::vector<std::string>&  wake_phrases,
                     float                            vad_threshold = 0.01f);
    ~WakeWordDetector();

    WakeWordDetector(const WakeWordDetector&)            = delete;
    WakeWordDetector& operator=(const WakeWordDetector&) = delete;

    void start();
    void stop();
    void pause();    // called by Speech2TextManager before it takes the mic
    void resume();   // called by Speech2TextManager when the full pipeline is done

private:
    void detect_loop();
    bool phrase_matches(const std::string& text) const;

    TriggerCallback         callback_;
    std::vector<std::string> wake_phrases_; // each stored lower-case, punctuation-free
    float                   vad_threshold_;
    std::atomic<bool>       running_;
    std::thread             thread_;
    whisper_context*        ctx_;

    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    stt_active_;
};
