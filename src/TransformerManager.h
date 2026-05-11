#pragma once

#include <string>

#include <nlohmann/json.hpp>

class TransformerManager {
public:
    explicit TransformerManager(const std::string& model);

    std::string chat(const std::string& user_text);

private:
    std::string    model_;
    nlohmann::json history_;
};
