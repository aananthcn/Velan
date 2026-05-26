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
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <portaudio.h>
#include <grpcpp/grpcpp.h>

#include "VehicleServer.grpc.pb.h"
#include "VehicleServer.pb.h"

#include "log.h"
#include "Speech2TextManager.h"
#include "Text2SpeechManager.h"
#include "TransformerManager.h"
#include "WakeWordDetector.h"
#include "WhisperTranscriber.h"
#ifdef GGML_USE_HAILO8
#include "HailoTranscriber.h"
#endif

namespace vhal = ::android::hardware::automotive::vehicle::proto;

// ---------------------------------------------------------------------------
// Config — override via CLI: ./velan [OPTIONS]
// ---------------------------------------------------------------------------
static const char* DEFAULT_STT_MODEL      = "models/stt/ggml-medium.bin"; // Whisper C++
static const char* DEFAULT_TTS_MODEL      = "models/tts/en_US-lessac-medium.onnx"; // Piper
static const char* DEFAULT_OLLAMA_MODEL   = "llama3.2:3b";
static const char* DEFAULT_VHAL_SERVER    = "localhost:50051";
static const char* DEFAULT_WAKEWORD_PHRASE = WWD_DEFAULT_WAKE_WORDS;
static const char* DEFAULT_ENCODER_HEF    = "models/stt/whisper-tiny-h8l/encoder.hef";
static const char* DEFAULT_DECODER_HEF    = "models/stt/whisper-tiny-h8l/decoder.hef";
static const char* DEFAULT_VOCAB_JSON     = "models/stt/whisper-tiny-h8l/weights/vocab.json";


// Poll interval for GetValues (milliseconds). 10 Hz matches vhal-gateway.
static const int POLL_INTERVAL_MS = 100;


// Application-level singletons — one of each for the process lifetime.
static std::unique_ptr<ITranscriber>       g_transcriber;
static std::unique_ptr<TransformerManager> g_llm;
static std::unique_ptr<Text2SpeechManager> g_tts;
static std::unique_ptr<WakeWordDetector>   g_wwd;


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
    std::string stt_model       = DEFAULT_STT_MODEL;
    std::string tts_model       = DEFAULT_TTS_MODEL;
    std::string ollama_model    = DEFAULT_OLLAMA_MODEL;
    std::string ollama_host     = "localhost";  // override with --llm <host> for remote Ollama
    std::string vhal_server     = DEFAULT_VHAL_SERVER;
    std::string wakeword_phrase = DEFAULT_WAKEWORD_PHRASE;
    std::string aicore          = "whisper";    // "whisper" | "hailo8"
    std::string encoder_hef     = DEFAULT_ENCODER_HEF;
    std::string decoder_hef     = DEFAULT_DECODER_HEF;
    std::string vocab_json      = DEFAULT_VOCAB_JSON;
    int         mic_device      = -1;   // -1 = PortAudio system default; override with --mic
    std::string tts_sink        = "";   // ALSA device for aplay fallback (e.g. "plughw:0,0")
    std::string test_wav        = "";   // if non-empty: transcribe this WAV file and exit
};


// ---------------------------------------------------------------------------
// CLI handling
// ---------------------------------------------------------------------------
// Print all PortAudio input devices. Pa_Initialize() must have been called.
static void list_input_devices() {
    int n = Pa_GetDeviceCount();
    if (n <= 0) { std::cout << "  (none found)\n"; return; }
    PaDeviceIndex def = Pa_GetDefaultInputDevice();
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
        if (!d || d->maxInputChannels < 1) continue;
        std::cout << "  [" << i << "] " << d->name
                  << "  (" << d->maxInputChannels << " ch, "
                  << static_cast<int>(d->defaultSampleRate) << " Hz"
                  << (i == def ? ", default" : "") << ")\n";
    }
}

static void print_help(const char* prog) {
    std::cout
        << "Usage: " << prog << " [OPTIONS]\n\n"
        << "Options:\n"
        << "  --server      <host>    VHAL gRPC server address\n"
        << "  --sttmodel    <path>    Whisper STT model path (whisper aicore only)\n"
        << "  --ttsmodel    <path>    Piper TTS model path\n"
        << "  --llmodel     <name>    Ollama model name\n"
        << "  --llm         <host>    Ollama server host (default: localhost)\n"
        << "  --wwphrase    <phrases> Comma-separated wake phrases\n"
        << "  --aicore      <mode>    Transcription backend: whisper (default) | hailo8\n"
        << "  --encoder-hef <path>    Hailo encoder HEF path (hailo8 only)\n"
        << "  --decoder-hef <path>    Hailo decoder HEF path (hailo8 only)\n"
        << "  --vocab-json  <path>    Whisper vocab JSON for detokenisation (hailo8 only)\n"
        << "  --mic         <index>   PortAudio input device index\n"
        << "  --list-mic              Print available microphone devices and exit\n"
        << "  --tts-sink    <device>  ALSA device for TTS aplay fallback (e.g. plughw:0,0)\n"
        << "  --test-wav  <path>      Transcribe a WAV file (16 kHz mono PCM) and exit\n"
        << "  --help                  Show this help\n\n"
        << "Defaults:\n"
        << "  STT model     : " << DEFAULT_STT_MODEL      << "\n"
        << "  TTS model     : " << DEFAULT_TTS_MODEL      << "\n"
        << "  LLM host      : localhost\n"
        << "  VHAL server   : " << DEFAULT_VHAL_SERVER    << "\n"
        << "  Wake phrase   : " << DEFAULT_WAKEWORD_PHRASE << "\n"
        << "  Encoder HEF   : " << DEFAULT_ENCODER_HEF    << "\n"
        << "  Decoder HEF   : " << DEFAULT_DECODER_HEF    << "\n"
        << "  Vocab JSON    : " << DEFAULT_VOCAB_JSON      << "\n"
        << "  Mic device    : system default\n";
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
        else if (std::strcmp(argv[i], "--llm") == 0 ||
                 std::strcmp(argv[i], "--llmhost") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for " << argv[i] << "\n";
                return false;
            }
            cfg.ollama_host = argv[++i];
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
        else if (std::strcmp(argv[i], "--aicore") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --aicore\n";
                return false;
            }
            cfg.aicore = argv[++i];
        }
        else if (std::strcmp(argv[i], "--encoder-hef") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --encoder-hef\n";
                return false;
            }
            cfg.encoder_hef = argv[++i];
        }
        else if (std::strcmp(argv[i], "--decoder-hef") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --decoder-hef\n";
                return false;
            }
            cfg.decoder_hef = argv[++i];
        }
        else if (std::strcmp(argv[i], "--vocab-json") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --vocab-json\n";
                return false;
            }
            cfg.vocab_json = argv[++i];
        }
        else if (std::strcmp(argv[i], "--mic") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --mic\n";
                return false;
            }
            cfg.mic_device = std::stoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--tts-sink") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --tts-sink\n";
                return false;
            }
            cfg.tts_sink = argv[++i];
        }
        else if (std::strcmp(argv[i], "--test-wav") == 0) {
            if ((i + 1) >= argc) {
                std::cerr << "[Velan] Missing value for --test-wav\n";
                return false;
            }
            cfg.test_wav = argv[++i];
        }
        else if (std::strcmp(argv[i], "--list-mic") == 0) {
            Pa_Initialize();
            std::cout << "Available audio input devices:\n";
            list_input_devices();
            Pa_Terminate();
            return false;   // exit after listing
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
// Minimal WAV reader — PCM only (format tag = 1).
// Accepts 16-bit signed, 8-bit unsigned, or 32-bit float samples.
// Takes the first (left) channel if stereo; warns if not 16 kHz.
// Returns float32 PCM normalised to [-1.0, 1.0].
// ---------------------------------------------------------------------------
static std::vector<float> read_wav_pcm(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open WAV file: " + path);

    auto rd16 = [&]() -> uint16_t {
        uint16_t v = 0; f.read(reinterpret_cast<char*>(&v), 2); return v; };
    auto rd32 = [&]() -> uint32_t {
        uint32_t v = 0; f.read(reinterpret_cast<char*>(&v), 4); return v; };

    // RIFF header
    char riff[4] = {}; f.read(riff, 4);
    if (std::strncmp(riff, "RIFF", 4) != 0)
        throw std::runtime_error("Not a RIFF file: " + path);
    rd32(); // file size (ignored)
    char wave[4] = {}; f.read(wave, 4);
    if (std::strncmp(wave, "WAVE", 4) != 0)
        throw std::runtime_error("Not a WAVE file: " + path);

    uint16_t audio_fmt = 0, channels = 0, bps = 0;
    uint32_t sample_rate = 0;
    bool     found_fmt   = false;

    char id[4] = {};
    while (f.read(id, 4)) {
        uint32_t sz = rd32();
        if (std::strncmp(id, "fmt ", 4) == 0) {
            audio_fmt   = rd16();
            channels    = rd16();
            sample_rate = rd32();
            rd32();  // byte rate
            rd16();  // block align
            bps         = rd16();
            if (sz > 16) f.seekg(sz - 16, std::ios::cur);
            found_fmt = true;
        } else if (std::strncmp(id, "data", 4) == 0) {
            if (!found_fmt)
                throw std::runtime_error("WAV data chunk before fmt chunk");
            // audio_fmt 1 = PCM, 3 = IEEE float
            if (audio_fmt != 1 && audio_fmt != 3)
                throw std::runtime_error("WAV must be PCM (format=1) or IEEE float (format=3); got " +
                                         std::to_string(audio_fmt));
            if (channels == 0)
                throw std::runtime_error("WAV fmt has 0 channels");
            if (sample_rate != 16000)
                std::cerr << "[test-wav] WARNING: sample rate=" << sample_rate
                          << " Hz (need 16000). Convert with:\n"
                          << "  sox input.wav -r 16000 -c 1 -b 16 output.wav\n";

            std::vector<uint8_t> raw(sz);
            f.read(reinterpret_cast<char*>(raw.data()), sz);

            int bytes_per_samp = bps / 8;
            int frame_bytes    = bytes_per_samp * channels;
            int n_frames       = (frame_bytes > 0) ? sz / frame_bytes : 0;

            std::vector<float> pcm;
            pcm.reserve(n_frames);
            for (int i = 0; i < n_frames; ++i) {
                const uint8_t* s = raw.data() + (size_t)i * frame_bytes; // first channel
                float v = 0.0f;
                if (audio_fmt == 3 && bps == 32) {           // IEEE float32
                    std::memcpy(&v, s, 4);
                } else if (bps == 16) {                       // signed 16-bit
                    int16_t s16; std::memcpy(&s16, s, 2);
                    v = s16 / 32768.0f;
                } else if (bps == 8) {                        // unsigned 8-bit
                    v = (s[0] - 128) / 128.0f;
                } else if (bps == 32) {                       // signed 32-bit
                    int32_t s32; std::memcpy(&s32, s, 4);
                    v = s32 / 2147483648.0f;
                }
                pcm.push_back(v);
            }
            return pcm;
        } else {
            f.seekg(sz, std::ios::cur);  // skip unknown chunk
        }
    }
    throw std::runtime_error("No data chunk found in WAV: " + path);
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
// Run the LLM + TTS pipeline, then resume wake-word detection.
// Called by Speech2TextManager via the on_transcript callback after Whisper
// finishes transcribing. Empty text means silence/error — still resumes WWD.
// ---------------------------------------------------------------------------
static void chat_with_ai_model(const std::string& text) {
    if (!text.empty()) {
        if (g_llm) {
            try {
                std::cout << log_ts() << "[Velan] Thinking...\n";
                std::string reply = g_llm->chat(text);
                std::cout << log_ts() << "[Velan] Assistant: " << reply << "\n\n";
                g_tts->speak(reply);
            } catch (const std::exception& e) {
                std::cerr << log_ts() << "[Velan] Ollama error: " << e.what() << "\n";
            }
        } else {
            std::cout << log_ts() << "[Velan] Transcript (no LLM): " << text << "\n";
        }
    }
    if (g_wwd) g_wwd->resume();     // PROCESSING → LISTENING
}


// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Ensure every log line appears immediately even when stdout is a pipe (SSH).
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;

    VelanConfigs cfg;

    if (!parse_cmdline(argc, argv, cfg)) {
        return 1;
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGABRT, on_signal);
    std::signal(SIGHUP,  on_signal);   // SSH disconnect → clean exit

    std::cout << log_ts() << "[Velan] ======== VelanConfigs ========\n";
    std::cout << log_ts() << "[Velan] AI core        : " << cfg.aicore        << "\n";
    if (cfg.aicore == "hailo8") {
        std::cout << log_ts() << "[Velan] Encoder HEF    : " << cfg.encoder_hef  << "\n";
        std::cout << log_ts() << "[Velan] Decoder HEF    : " << cfg.decoder_hef  << "\n";
        std::cout << log_ts() << "[Velan] Vocab JSON     : " << cfg.vocab_json   << "\n";
    } else {
        std::cout << log_ts() << "[Velan] STT/WWD model  : " << cfg.stt_model    << "\n";
    }
    std::cout << log_ts() << "[Velan] TTS model      : " << cfg.tts_model      << "\n";
    std::cout << log_ts() << "[Velan] LLM model      : " << cfg.ollama_model   << "\n";
    std::cout << log_ts() << "[Velan] LLM host       : " << cfg.ollama_host    << "\n";
    std::cout << log_ts() << "[Velan] VHAL server    : " << cfg.vhal_server    << "\n";
    std::cout << log_ts() << "[Velan] Wake phrase    : " << cfg.wakeword_phrase << "\n";
    if (cfg.mic_device < 0)
        std::cout << log_ts() << "[Velan] Mic device     : system default\n";
    else
        std::cout << log_ts() << "[Velan] Mic device     : " << cfg.mic_device << "\n";
    if (cfg.tts_sink.empty())
        std::cout << log_ts() << "[Velan] TTS ALSA sink  : system default\n";
    else
        std::cout << log_ts() << "[Velan] TTS ALSA sink  : " << cfg.tts_sink << "\n";
    std::cout << log_ts() << "[Velan] =====================================\n";

    // ---- Transcriber: one model instance shared by STT and WWD ----
    try {
#ifdef GGML_USE_HAILO8
        if (cfg.aicore == "hailo8") {
            g_transcriber = std::make_unique<HailoTranscriber>(
                cfg.encoder_hef, cfg.decoder_hef, cfg.vocab_json);
        } else
#endif
        {
            if (cfg.aicore != "whisper" && cfg.aicore != "cpu" && cfg.aicore != "cuda") {
                std::cerr << "[Velan] Unknown --aicore value '" << cfg.aicore
                          << "'. Valid: whisper, hailo8\n";
                return 1;
            }
            g_transcriber = std::make_unique<WhisperTranscriber>(cfg.stt_model.c_str());
        }
    } catch (const std::exception& e) {
        std::cerr << "[Velan] " << e.what() << "\n";
        return 1;
    }

    // ---- Test mode: transcribe a WAV file and exit (no mic / VHAL / TTS needed) ----
    if (!cfg.test_wav.empty()) {
        try {
            std::cout << log_ts() << "[test-wav] Reading: " << cfg.test_wav << "\n";
            auto pcm = read_wav_pcm(cfg.test_wav);
            std::cout << log_ts() << "[test-wav] " << pcm.size() << " samples ("
                      << std::fixed << std::setprecision(2)
                      << pcm.size() / 16000.0f << "s at 16 kHz)\n";
            std::string result = g_transcriber->transcribe(pcm, /*single_segment=*/false);
            std::cout << log_ts() << "[test-wav] Transcription: \"" << result << "\"\n";
        } catch (const std::exception& e) {
            std::cerr << "[test-wav] Error: " << e.what() << "\n";
            g_transcriber.reset();  // clean shutdown before exit
            return 1;
        }
        // Explicitly reset transcriber before exit so HailoRT can shut down
        // cleanly while the main stack frame is still valid. Without this, the
        // HailoRT driver may have already torn down its internal state before
        // the global unique_ptr destructor runs, causing a segfault.
        g_transcriber.reset();
        return 0;
    }

    // ---- Normal runtime: init TTS, LLM, STT manager, WWD, VHAL loop ----
    g_llm = std::make_unique<TransformerManager>(cfg.ollama_model, cfg.ollama_host);
    g_tts = std::make_unique<Text2SpeechManager>(cfg.tts_model, cfg.tts_sink);

    // ---- STT: uses the shared transcriber ----
    Speech2TextManager* mgr_ptr;
    try {
        mgr_ptr = &Speech2TextManager::instance(
            g_transcriber.get(),
            []()                        { if (g_wwd) g_wwd->pause(); },   // LISTENING → PROCESSING
            [](const std::string& text) { chat_with_ai_model(text);  },   // transcript → LLM → TTS → resume
            cfg.mic_device
        );
    } catch (const std::exception& e) {
        std::cerr << "[Velan] " << e.what() << "\n";
        return 1;
    }
    Speech2TextManager& mgr = *mgr_ptr;

    // ---- WWD: shares the same transcriber — no second model loaded ----
    try {
        g_wwd = std::make_unique<WakeWordDetector>(
            [&mgr]() { mgr.handle_trigger(); },        // LISTENING → PROCESSING
            g_transcriber.get(),
            split_phrases(cfg.wakeword_phrase),
            0.01f,
            cfg.mic_device
        );
        g_wwd->start();
    } catch (const std::exception& e) {
        std::cerr << "[Velan] Wake word detector failed to start: " << e.what() << "\n";
        std::cerr << "[Velan] Continuing with VHAL trigger only.\n";
        g_wwd.reset();
    }

    // ---- VHAL gRPC polling loop ----
    // Uses GetValues at 10 Hz — same pattern as vhal-gateway.
    // This avoids StartPropertyValuesStream's server-side write failures when
    // FakeVehicleHardware pushes updates from a different thread.
    std::cout << log_ts() << "[Velan] Polling VOICE_ASSIST_TRIGGER on VHAL at " << cfg.vhal_server << "\n";

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

    if (g_wwd) g_wwd->stop();

    std::cout << "[Velan] Bye.\n";
    return 0;
}
