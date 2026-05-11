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
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "VehicleServer.grpc.pb.h"
#include "VehicleServer.pb.h"

#include "Speech2TextManager.h"

namespace vhal = ::android::hardware::automotive::vehicle::proto;

// ---------------------------------------------------------------------------
// Config — override via CLI: ./velan [OPTIONS]
// ---------------------------------------------------------------------------
static const char* DEFAULT_WHISPER_MODEL = "models/stt/ggml-medium.bin";
static const char* DEFAULT_TTS_MODEL     = "models/tts/en_US-lessac-medium.onnx";
static const char* DEFAULT_OLLAMA_MODEL  = "llama3.2:3b";
static const char* DEFAULT_VHAL_SERVER   = "localhost:50051";

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
struct Config {
    std::string whisper_model = DEFAULT_WHISPER_MODEL;
    std::string tts_model     = DEFAULT_TTS_MODEL;
    std::string ollama_model  = DEFAULT_OLLAMA_MODEL;
    std::string vhal_server   = DEFAULT_VHAL_SERVER;
};


// ---------------------------------------------------------------------------
// CLI handling
// ---------------------------------------------------------------------------
static void print_help(const char* prog) {
    std::cout
        << "Usage: " << prog << " [OPTIONS]\n\n"
        << "Options:\n"
        << "  --vmodel <WHISPER_MODEL>   Whisper STT model path\n"
        << "  --tts    <TTS_MODEL>       Piper TTS model path\n"
        << "  --llm    <OLLAMA_MODEL>    Ollama model name\n"
        << "  --server <VHAL_SERVER>     VHAL gRPC server address\n"
        << "  --help                     Show this help\n\n"
        << "Defaults:\n"
        << "  Whisper model : " << DEFAULT_WHISPER_MODEL << "\n"
        << "  TTS model     : " << DEFAULT_TTS_MODEL     << "\n"
        << "  Ollama model  : " << DEFAULT_OLLAMA_MODEL  << "\n"
        << "  VHAL server   : " << DEFAULT_VHAL_SERVER   << "\n";
}

static bool parse_cmdline(int argc, char* argv[], Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vmodel") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --vmodel\n";
                return false;
            }
            cfg.whisper_model = argv[++i];
        }
        else if (std::strcmp(argv[i], "--tts") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --tts\n";
                return false;
            }
            cfg.tts_model = argv[++i];
        }
        else if (std::strcmp(argv[i], "--llm") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --llm\n";
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
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    Config cfg;

    if (!parse_cmdline(argc, argv, cfg)) {
        return 1;
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGABRT, on_signal);

    Speech2TextManager* mgr_ptr;
    try {
        mgr_ptr = &Speech2TextManager::instance(cfg.ollama_model,
                                                 cfg.whisper_model.c_str(),
                                                 cfg.tts_model.c_str());
    } catch (const std::exception& e) {
        std::cerr << "[Velan] " << e.what() << "\n";
        return 1;
    }
    Speech2TextManager& mgr = *mgr_ptr;

    std::cout << "[Velan] Ollama model : " << cfg.ollama_model  << "\n";
    std::cout << "[Velan] TTS model    : " << cfg.tts_model     << "\n";

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

            mgr.handle_trigger(state);
        }

        for (int i = 0; i < POLL_INTERVAL_MS / 10 && !g_interrupted; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "[Velan] Bye.\n";
    return 0;
}
