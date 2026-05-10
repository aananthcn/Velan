#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <portaudio.h>
#include <whisper.h>

#include "VehicleServer.grpc.pb.h"
#include "VehicleServer.pb.h"

using json = nlohmann::json;
namespace vhal = ::android::hardware::automotive::vehicle::proto;

// ---------------------------------------------------------------------------
// Config — override via CLI: ./velan [model_path] [ollama_model] [vhal_server]
// ---------------------------------------------------------------------------
static const char* DEFAULT_WHISPER_MODEL = "models/ggml-medium.bin";
static const char* DEFAULT_OLLAMA_MODEL  = "llama3.2";
static const char* DEFAULT_VHAL_SERVER   = "localhost:50051";
static const char* OLLAMA_CHAT_URL       = "http://localhost:11434/api/chat";
static const int   SAMPLE_RATE           = 16000;
static const int   CHUNK_FRAMES          = 1024;
static const int   WHISPER_THREADS       = 4;

// Poll interval for GetValues (milliseconds). 10 Hz matches vhal-gateway.
static const int POLL_INTERVAL_MS = 100;

// Vendor-defined VHAL property for voice assist trigger.
// Encoding: VehiclePropertyGroup::VENDOR (0x20000000)
//         | VehicleArea::GLOBAL          (0x01000000)
//         | VehiclePropertyType::INT32   (0x00400000)
//         | unique id                    (0x0001)
static const int32_t VOICE_ASSIST_TRIGGER = 0x21400001;

static volatile bool g_interrupted = false;
static void on_signal(int) { g_interrupted = true; }

// ---------------------------------------------------------------------------
// Audio recording — stops when stop_flag is set or g_interrupted fires
// ---------------------------------------------------------------------------
static std::vector<float> record_audio(std::atomic<bool>& stop_flag) {
    PaStream* stream = nullptr;
    PaError   err;

    err = Pa_OpenDefaultStream(&stream, 1, 0, paFloat32,
                               SAMPLE_RATE, CHUNK_FRAMES, nullptr, nullptr);
    if (err != paNoError)
        throw std::runtime_error(std::string("Pa_OpenDefaultStream: ") + Pa_GetErrorText(err));

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        Pa_CloseStream(stream);
        throw std::runtime_error(std::string("Pa_StartStream: ") + Pa_GetErrorText(err));
    }

    std::vector<float> samples;
    samples.reserve(SAMPLE_RATE * 30);

    std::array<float, CHUNK_FRAMES> buf;
    while (!g_interrupted && !stop_flag.load()) {
        Pa_ReadStream(stream, buf.data(), CHUNK_FRAMES);
        samples.insert(samples.end(), buf.begin(), buf.end());
    }

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    return samples;
}

// ---------------------------------------------------------------------------
// Whisper transcription
// ---------------------------------------------------------------------------
static std::string transcribe(whisper_context* ctx, const std::vector<float>& pcm) {
    if (pcm.empty()) return {};

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads           = WHISPER_THREADS;
    params.language            = "en";
    params.translate           = false;
    params.print_special       = false;
    params.print_progress      = false;
    params.print_realtime      = false;
    params.print_timestamps    = false;
    params.single_segment      = false;

    if (whisper_full(ctx, params, pcm.data(), static_cast<int>(pcm.size())) != 0)
        throw std::runtime_error("whisper_full() failed");

    std::string text;
    int n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; ++i) {
        const char* seg = whisper_full_get_segment_text(ctx, i);
        if (seg) text += seg;
    }
    return text;
}

// ---------------------------------------------------------------------------
// Ollama chat — maintains conversation history across turns
// ---------------------------------------------------------------------------
static size_t curl_append(char* ptr, size_t sz, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, sz * nmemb);
    return sz * nmemb;
}

static std::string ollama_chat(const std::string& ollama_model,
                               json& history,
                               const std::string& user_text) {
    history.push_back({{"role", "user"}, {"content", user_text}});

    json body = {
        {"model",    ollama_model},
        {"messages", history},
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

    std::string reply = j.value("/message/content"_json_pointer, "");
    history.push_back({{"role", "assistant"}, {"content", reply}});
    return reply;
}

// ---------------------------------------------------------------------------
// Process a captured audio buffer: transcribe then query Ollama
// ---------------------------------------------------------------------------
static void process_audio(const std::vector<float>& audio,
                          whisper_context* ctx,
                          const std::string& ollama_model,
                          json& history) {
    if (audio.size() < static_cast<size_t>(SAMPLE_RATE / 2)) {
        std::cout << "[Velan] Audio too short — skipping.\n";
        return;
    }

    std::cout << "[Velan] Transcribing...\n";
    std::string transcript;
    try {
        transcript = transcribe(ctx, audio);
    } catch (const std::exception& e) {
        std::cerr << "[Velan] Transcription error: " << e.what() << "\n";
        return;
    }

    if (transcript.empty()) {
        std::cout << "[Velan] (nothing transcribed)\n";
        return;
    }
    std::cout << "[Velan] You said: " << transcript << "\n";

    std::cout << "[Velan] Thinking...\n";
    try {
        std::string reply = ollama_chat(ollama_model, history, transcript);
        std::cout << "[Velan] Assistant: " << reply << "\n\n";
    } catch (const std::exception& e) {
        std::cerr << "[Velan] Ollama error: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    const char* model_path   = (argc > 1) ? argv[1] : DEFAULT_WHISPER_MODEL;
    const char* ollama_model = (argc > 2) ? argv[2] : DEFAULT_OLLAMA_MODEL;
    const char* vhal_server  = (argc > 3) ? argv[3] : DEFAULT_VHAL_SERVER;

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGABRT, on_signal);

    // ---- PortAudio ----
    if (Pa_Initialize() != paNoError) {
        std::cerr << "[Velan] PortAudio init failed\n";
        return 1;
    }

    // ---- Whisper ----
    whisper_context_params wparams = whisper_context_default_params();
#ifdef GGML_USE_CUDA
    wparams.use_gpu    = true;
    wparams.gpu_device = 0;
    std::cout << "[Velan] Loading Whisper model (GPU): " << model_path << "\n";
#else
    wparams.use_gpu    = false;
    std::cout << "[Velan] Loading Whisper model (CPU): " << model_path << "\n";
#endif
    whisper_context* ctx = whisper_init_from_file_with_params(model_path, wparams);
    if (!ctx) {
        std::cerr << "[Velan] Failed to load Whisper model from: " << model_path << "\n"
                  << "Download with:\n"
                  << "  mkdir -p models && curl -L --progress-bar \\\n"
                  << "    -o models/ggml-medium.bin \\\n"
                  << "    https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin\n";
        Pa_Terminate();
        return 1;
    }
    std::cout << "[Velan] Whisper model loaded.\n";

    // ---- Ollama conversation history ----
    json history = json::array();
    history.push_back({
        {"role",    "system"},
        {"content", "You are Velan, a concise and helpful voice assistant. "
                    "Keep replies short since they will be read aloud."}
    });
    std::cout << "[Velan] Ollama model: " << ollama_model << "\n";

    // ---- VHAL gRPC polling loop ----
    // Uses GetValues at 10 Hz — same pattern as vhal-gateway.
    // This avoids StartPropertyValuesStream's server-side write failures when
    // FakeVehicleHardware pushes updates from a different thread.
    std::cout << "[Velan] Polling VOICE_ASSIST_TRIGGER on VHAL at " << vhal_server << "\n";

    auto channel = grpc::CreateChannel(vhal_server, grpc::InsecureChannelCredentials());
    auto stub    = vhal::VehicleServer::NewStub(channel);

    // Build a reusable GetValues request for VOICE_ASSIST_TRIGGER
    vhal::VehiclePropValueRequests get_req;
    {
        auto* req = get_req.add_requests();
        req->set_request_id(1);
        req->mutable_value()->set_prop(VOICE_ASSIST_TRIGGER);
        req->mutable_value()->set_area_id(0);
    }

    int32_t            prev_state    = -1;   // -1 = unknown (server not yet read)
    bool               is_recording  = false;
    std::atomic<bool>  stop_recording{false};
    std::thread        rec_thread;
    std::vector<float> audio;

    while (!g_interrupted) {
        grpc::ClientContext get_ctx;
        get_ctx.set_deadline(
            std::chrono::system_clock::now() + std::chrono::milliseconds(500));

        vhal::GetValueResults get_results;
        grpc::Status s = stub->GetValues(&get_ctx, get_req, &get_results);

        if (!s.ok()) {
            if (prev_state != -1) {
                std::cerr << "[Velan] VHAL server not reachable at " << vhal_server
                          << " — is vhal-server running?\n";
                prev_state = -1;
            }
            for (int i = 0; i < POLL_INTERVAL_MS / 10 && !g_interrupted; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (prev_state == -1)
            std::cout << "[Velan] Connected to VHAL. Waiting for VOICE_ASSIST_TRIGGER events...\n";

        for (const auto& result : get_results.results()) {
            if (result.status() != vhal::StatusCode::OK) continue;
            if (!result.has_value()) continue;

            int32_t state = result.value().int32_values_size() > 0
                          ? result.value().int32_values(0) : 0;

            if (state == prev_state) continue;
            prev_state = state;

            if (state == 1 && !is_recording) {
                audio.clear();
                stop_recording = false;
                is_recording   = true;
                rec_thread = std::thread([&]() {
                    try {
                        audio = record_audio(stop_recording);
                    } catch (const std::exception& e) {
                        std::cerr << "[Velan] Recording error: " << e.what() << "\n";
                    }
                });
                std::cout << "[Velan] TRIGGER_ON  — recording started.\n";

            } else if (state == 0 && is_recording) {
                stop_recording = true;
                is_recording   = false;
                if (rec_thread.joinable()) rec_thread.join();
                std::cout << "[Velan] TRIGGER_OFF — recording stopped.\n";
                process_audio(audio, ctx, ollama_model, history);
            }
        }

        for (int i = 0; i < POLL_INTERVAL_MS / 10 && !g_interrupted; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Clean up if interrupted mid-recording
    if (is_recording) {
        stop_recording = true;
        if (rec_thread.joinable()) rec_thread.join();
    }

    whisper_free(ctx);
    Pa_Terminate();
    std::cout << "[Velan] Bye.\n";
    return 0;
}
