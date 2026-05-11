#pragma once

#include <string>

class Text2SpeechManager {
public:
    explicit Text2SpeechManager(const std::string& model_path);
    void speak(const std::string& text);

private:
    std::string model_;
};
