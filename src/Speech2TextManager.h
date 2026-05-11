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
