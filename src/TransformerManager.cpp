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

#include "TransformerManager.h"

#include <stdexcept>
#include <string>

#include <curl/curl.h>

using json = nlohmann::json;

static const char* OLLAMA_CHAT_URL = "http://localhost:11434/api/chat";


static size_t curl_append(char* ptr, size_t sz, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, sz * nmemb);
    return sz * nmemb;
}


TransformerManager::TransformerManager(const std::string& model)
    : model_(model) {
    history_ = json::array();
    history_.push_back({
        {"role",    "system"},
        {"content", "You are Velan, a concise and helpful voice assistant. "
                    "Keep replies short since they will be read aloud."}
    });
}


std::string TransformerManager::chat(const std::string& user_text) {
    history_.push_back({{"role", "user"}, {"content", user_text}});

    json body = {
        {"model",    model_},
        {"messages", history_},
        {"stream",   false}
    };
    std::string body_str = body.dump();
    std::string resp_buf;

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,           OLLAMA_CHAT_URL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_append);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp_buf);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));

    auto j = json::parse(resp_buf, nullptr, false);
    if (j.is_discarded())
        throw std::runtime_error("ollama: invalid JSON response");
    if (j.contains("error"))
        throw std::runtime_error("ollama: " + j["error"].get<std::string>());

    std::string reply = j.value("/message/content"_json_pointer, "");
    history_.push_back({{"role", "assistant"}, {"content", reply}});
    return reply;
}
