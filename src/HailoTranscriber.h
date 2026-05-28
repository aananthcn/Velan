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

#include <memory>
#include <string>
#include <vector>

// Whisper transcription backend using the Hailo-8 NPU via HailoRT.
//
// Requires two HEF files compiled for Hailo-8L from a Whisper ONNX export:
//   encoder.hef  — mel spectrogram → encoder hidden states (one shot per utterance)
//   decoder.hef  — encoder output + token prefix → next-token logits (autoregressive)
//
// To compile HEF files from an ONNX Whisper export:
//   pip install openai-whisper onnx
//   python3 -c "import whisper; whisper.export_onnx('medium', 'whisper_medium')"
//   hailo compile whisper_medium_encoder.onnx --target hailo8l -o models/stt/encoder.hef
//   hailo compile whisper_medium_decoder.onnx --target hailo8l -o models/stt/decoder.hef
//
// The vocab JSON required for detokenisation can be extracted with:
//   python3 -c "import whisper, json; tok = whisper.tokenizer.get_tokenizer(True);
//               json.dump({int(v): str(tok.decode([v])) for v in range(tok.eot+1)},
//               open('models/stt/vocab.json','w'))"
//
// pimpl keeps hailort.hpp out of this header, so callers don't need the SDK.
class HailoTranscriber : public ITranscriber {
public:
    HailoTranscriber(const std::string& encoder_hef_path,
                     const std::string& decoder_hef_path,
                     const std::string& vocab_json_path = "models/stt/vocab.json");
    ~HailoTranscriber() override;

    HailoTranscriber(const HailoTranscriber&)            = delete;
    HailoTranscriber& operator=(const HailoTranscriber&) = delete;

    std::string transcribe(const std::vector<float>& pcm,
                           bool               single_segment = false,
                           int                audio_ctx      = 0,
                           const std::string& initial_prompt = "") override;
                           // initial_prompt: accepted for interface compatibility;
                           // not applicable to the HEF/NPU decode path.

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
