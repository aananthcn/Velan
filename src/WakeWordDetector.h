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
#include <condition_variable>
#include <functional>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Default wake phrases / patterns.
//
// Phrases prefixed with "regex:" are compiled as ECMAScript case-insensitive
// regular expressions and matched against the normalised Whisper transcript
// via std::regex_search.  All other phrases use the existing case-insensitive
// substring match.
//
// Pattern rationale for "Subramanya":
//   The second syllable starts with 'b', so the common prefix is "su" and the
//   alternatives are (bro|bra|brah):
//     su + bra  + man + ya  →  subramanya
//     su + brah + man + ya  →  subrahmanya   (Whisper inserts silent 'h')
//     su + bro  + man + ya  →  subromanya    (rare Whisper variant)
//   Endings: (i|ya) — covers both "Subramani" and "Subramanya" spellings.
//   A second pattern covers the 'p'-consonant variants Whisper occasionally
//   produces (supramanya, suprahmanya, etc.).
#define WWD_DEFAULT_WAKE_WORDS \
    "regex:Hey (V|B)ell?a(h|n), " \
    "regex:su(bro|bra|brah)man(i|ya), " \
    "regex:su(pro|pra|prah)man(i|ya)"

// Continuously listens on the microphone in LISTENING state.
// When the wake phrase is detected, fires callback() and blocks until
// resume() is called — signalling that STT/Chat/TTS have finished.
// The two states (LISTENING / PROCESSING) are mutually exclusive: only
// one component owns the microphone at a time.
//
// Shares the ITranscriber* owned by main() — no second model is loaded.
// Safe because LISTENING and PROCESSING never overlap.
class WakeWordDetector {
public:
    using TriggerCallback = std::function<void()>;

    // wake_phrases: one or more phrases; any match fires the trigger.
    // transcriber: shared transcription backend (WhisperTranscriber or HailoTranscriber).
    // mic_device:  PortAudio device index, or -1 to use the system default.
    WakeWordDetector(TriggerCallback                  cb,
                     ITranscriber*                    transcriber,
                     const std::vector<std::string>&  wake_phrases,
                     float                            vad_threshold = 0.01f,
                     int                              mic_device    = -1);
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

    TriggerCallback          callback_;
    ITranscriber*            transcriber_;   // non-owning; owned by main()
    std::vector<std::string> wake_phrases_;  // plain substrings — lower-case, punct-free

    // Regex patterns — compiled once in the constructor.
    // Stored as (source_string, compiled_regex) pairs so the source is
    // available for logging without recompiling.
    std::vector<std::pair<std::string, std::regex>> wake_patterns_;

    float                    vad_threshold_;
    int                      mic_device_;    // PortAudio device index; -1 = default
    std::atomic<bool>        running_;
    std::thread              thread_;

    std::mutex               mutex_;
    std::condition_variable  cv_;
    bool                     stt_active_;
};
