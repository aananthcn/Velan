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

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "VehicleServer.grpc.pb.h"
#include "VehicleServer.pb.h"

#include "Speech2TextManager.h"
#include "WakeWordDetector.h"

namespace vhal = ::android::hardware::automotive::vehicle::proto;

// ---------------------------------------------------------------------------
// Config — override via CLI: ./velan [OPTIONS]
// ---------------------------------------------------------------------------
static const char* DEFAULT_WHISPER_MODEL  = "models/stt/ggml-medium.bin";
static const char* DEFAULT_TTS_MODEL      = "models/tts/en_US-lessac-medium.onnx";
static const char* DEFAULT_OLLAMA_MODEL   = "llama3.2:3b";
static const char* DEFAULT_VHAL_SERVER    = "localhost:50051";
static const char* DEFAULT_WAKEWORD_PHRASE = WWD_DEFAULT_WAKE_WORD;

// Poll interval for GetValues (milliseconds). 10 Hz matches vhal-gateway.
static const int POLL_INTERVAL_MS = 100;

// Vendor-defined VHAL property for voice assistant trigger.
// Encoding: VehiclePropertyGroup::VENDOR (0x20000000)
//         | VehicleArea::GLOBAL          (0x01000000)
//         | VehiclePropertyType::INT32   (0x00400000)
//         | unique id                    (0x0001)
static const int32_t VOICE_ASSIST_TRIGGER = 0x21400001;


// ---------------------------------------------------------------------------
// Runtime configuration
// ---------------------------------------------------------------------------
struct VelanConfigs {
    std::string stt_model       = DEFAULT_WHISPER_MODEL;
    std::string tts_model       = DEFAULT_TTS_MODEL;
    std::string ollama_model    = DEFAULT_OLLAMA_MODEL;
    std::string vhal_server     = DEFAULT_VHAL_SERVER;
    std::string wakeword_phrase = DEFAULT_WAKEWORD_PHRASE;
};


// ---------------------------------------------------------------------------
// CLI handling
// ---------------------------------------------------------------------------
static void print_help(const char* prog) {
    std::cout
        << "Usage: " << prog << " [OPTIONS]\n\n"
        << "Options:\n"
        << "  --server    <host>    VHAL gRPC server address\n"
        << "  --sttmodel  <path>    Whisper STT model path\n"
        << "  --ttsmodel  <path>    Piper TTS model path\n"
        << "  --llmodel   <name>    Ollama model name\n"
        << "  --wwphrase  <phrases> Comma-separated wake phrases, e.g. \"Subramanya,Hey Vela\"\n"
        << "  --help                Show this help\n\n"
        << "Defaults:\n"
        << "  STT model       : " << DEFAULT_WHISPER_MODEL   << "\n"
        << "  TTS model       : " << DEFAULT_TTS_MODEL       << "\n"
        << "  LLM model       : " << DEFAULT_OLLAMA_MODEL    << "\n"
        << "  VHAL server     : " << DEFAULT_VHAL_SERVER     << "\n"
        << "  Wake phrase     : " << DEFAULT_WAKEWORD_PHRASE << "\n";
}

static bool parse_cmdline(int argc, char* argv[], VelanConfigs& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--sttmodel") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --sttmodel\n";
                return false;
            }
            cfg.stt_model = argv[++i];
        }
        else if (std::strcmp(argv[i], "--ttsmodel") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --ttsmodel\n";
                return false;
            }
            cfg.tts_model = argv[++i];
        }
        else if (std::strcmp(argv[i], "--llmodel") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --llmodel\n";
                return false;
            }
            cfg.ollama_model = argv[++i];
        }
        else if (std::strcmp(argv[i], "--server") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --server\n";
                return false;
            }
            cfg.vhal_server = argv[++i];
        }
        else if (std::strcmp(argv[i], "--wwphrase") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --wwphrase\n";
                return false;
            }
            cfg.wakeword_phrase = argv[++i];
        }
        else if (std::strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return false;
        }
        else {
            std::cerr << "[Velan] Unknown argument: " << argv[i] << "\n";
            print_help(argv[0]);
            return false;
        }
    }

    return true;
}


volatile bool g_interrupted = false;
static void on_signal(int) {
    g_interrupted = true;
}

// ---------------------------------------------------------------------------
// Split a comma-separated phrase list into a trimmed vector.
// Example: "Subramanya, Hey Vela" → ["subramanya", "hey vela"]
// ---------------------------------------------------------------------------
static std::vector<std::string> split_phrases(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        auto start = token.find_first_not_of(" \t");
        auto end   = token.find_last_not_of(" \t");
        if (start != std::string::npos)
            result.push_back(token.substr(start, end - start + 1));
    }
    return result;
}


// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    VelanConfigs cfg;

    if (!parse_cmdline(argc, argv, cfg)) {
        return 1;
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGABRT, on_signal);

    std::cout << "[Velan] ======== VelanConfigs ========\n";
    std::cout << "[Velan] STT/WWD model  : " << cfg.stt_model      << "\n";
    std::cout << "[Velan] TTS model      : " << cfg.tts_model      << "\n";
    std::cout << "[Velan] LLM model      : " << cfg.ollama_model   << "\n";
    std::cout << "[Velan] VHAL server    : " << cfg.vhal_server    << "\n";
    std::cout << "[Velan] Wake phrase    : " << cfg.wakeword_phrase << "\n";
    std::cout << "[Velan] =====================================\n";

    // wwd declared before STT so the on_start/on_done lambdas can capture it.
    std::unique_ptr<WakeWordDetector> wwd;

    // ---- STT: loads the Whisper model (context shared with WWD) ----
    Speech2TextManager* mgr_ptr;
    try {
        mgr_ptr = &Speech2TextManager::instance(
            cfg.ollama_model,
            cfg.stt_model.c_str(),
            cfg.tts_model.c_str(),
            [&wwd]() { if (wwd) wwd->pause(); },       // LISTENING → PROCESSING
            [&wwd]() { if (wwd) wwd->resume(); }       // PROCESSING → LISTENING
        );
    } catch (const std::exception& e) {
        std::cerr << "[Velan] " << e.what() << "\n";
        return 1;
    }
    Speech2TextManager& mgr = *mgr_ptr;

    // ---- WWD: shares STT's whisper_context — no second model loaded ----
    try {
        wwd = std::make_unique<WakeWordDetector>(
            [&mgr]() { mgr.handle_trigger(); },        // LISTENING → PROCESSING
            mgr.get_context(),
            split_phrases(cfg.wakeword_phrase),
            0.01f
        );
        wwd->start();
    } catch (const std::exception& e) {
        std::cerr << "[Velan] Wake word detector failed to start: " << e.what() << "\n";
        std::cerr << "[Velan] Continuing with VHAL trigger only.\n";
        wwd.reset();
    }

    // ---- VHAL gRPC polling loop ----
    // Uses GetValues at 10 Hz — same pattern as vhal-gateway.
    // This avoids StartPropertyValuesStream's server-side write failures when
    // FakeVehicleHardware pushes updates from a different thread.
    std::cout << "[Velan] Polling VOICE_ASSIST_TRIGGER on VHAL at " << cfg.vhal_server << "\n";

    auto channel = grpc::CreateChannel(cfg.vhal_server, grpc::InsecureChannelCredentials());
    auto stub    = vhal::VehicleServer::NewStub(channel);

    // Build a reusable GetValues request for VOICE_ASSIST_TRIGGER
    vhal::VehiclePropValueRequests get_req;
    {
        auto* req = get_req.add_requests();
        req->set_request_id(1);
        req->mutable_value()->set_prop(VOICE_ASSIST_TRIGGER);
        req->mutable_value()->set_area_id(0);
    }

    int32_t prev_state = -1;   // -1 = unknown (server not yet read)

    while (!g_interrupted) {
        grpc::ClientContext get_ctx;
        get_ctx.set_deadline(
            std::chrono::system_clock::now() + std::chrono::milliseconds(500));

        vhal::GetValueResults get_results;
        grpc::Status s = stub->GetValues(&get_ctx, get_req, &get_results);

        if (!s.ok()) {
            if (prev_state != -1) {
                std::cerr << "[Velan] VHAL server not reachable at " << cfg.vhal_server
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

            if (state == 1) mgr.handle_trigger();  // STT manages its own stop via VAD
        }

        for (int i = 0; i < POLL_INTERVAL_MS / 10 && !g_interrupted; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (wwd) wwd->stop();

    std::cout << "[Velan] Bye.\n";
    return 0;
}
