#!/usr/bin/env python3
# Copyright 2026 Aananth C N
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""
extract_whisper_weights.py  —  extract matmul-split weights and vocab from
a Whisper model for use with HailoTranscriber.

Usage:
    python3 scripts/extract_whisper_weights.py --model tiny --out models/stt

Outputs (all in --out directory):
    token_embedding_weight_<model>.npy   [vocab_size × d_model]  float32
    onnx_add_input_<model>.npy           [max_ctx × d_model]     float32
    vocab.json                           {token_id: utf8_string}

These files are loaded at startup by HailoTranscriber when running with
--aicore hailo8.  Without them the decoder receives raw float token IDs
instead of proper embeddings and will produce garbage output.

Why matmul-split?
    The Hailo-8L NPU does not natively support large embedding-lookup tables.
    The Whisper HEFs are compiled with the token embedding (decoder input) and
    output projection (decoder hidden→logits) offloaded to the CPU.  The NPU
    handles only the transformer attention layers.  This script extracts the
    two weight tensors that perform those CPU-side operations.
"""

import argparse
import json
import os
import sys

import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--model", default="tiny",
                        choices=["tiny", "base", "small", "medium", "large"],
                        help="Whisper model size (must match the HEF files)")
    parser.add_argument("--out", default="models/stt",
                        help="Output directory for .npy and vocab.json files")
    parser.add_argument("--multilingual", action="store_true", default=True,
                        help="Use multilingual tokenizer (default: True)")
    args = parser.parse_args()

    try:
        import whisper
    except ImportError:
        print("ERROR: openai-whisper not installed.  Run: pip install openai-whisper",
              file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.out, exist_ok=True)

    print(f"Loading whisper {args.model} model...")
    model = whisper.load_model(args.model)
    model = model.cpu()

    # ---- Token embedding weight: [vocab_size × d_model] ----
    emb = model.decoder.token_embedding.weight.detach().cpu().float().numpy()
    emb_path = os.path.join(args.out, f"token_embedding_weight_{args.model}.npy")
    np.save(emb_path, emb)
    print(f"Saved token embedding weight: {emb.shape} → {emb_path}")

    # ---- Positional embedding: [max_ctx × d_model] ----
    pos = model.decoder.positional_embedding.detach().cpu().float().numpy()
    pos_path = os.path.join(args.out, f"onnx_add_input_{args.model}.npy")
    np.save(pos_path, pos)
    print(f"Saved positional embedding:   {pos.shape} → {pos_path}")

    # ---- Vocabulary JSON ----
    # Use ensure_ascii=False so non-ASCII characters are stored as UTF-8 bytes
    # rather than \uXXXX escape sequences.  The C++ load_vocab() function uses
    # nlohmann::json which handles both forms, but ensure_ascii=False keeps the
    # file human-readable and avoids any escape-sequence edge cases.
    tok = whisper.tokenizer.get_tokenizer(args.multilingual)
    vocab = {}
    failed = 0
    for v in range(tok.eot + 1):
        try:
            text = tok.decode([v])
            vocab[int(v)] = text
        except Exception:
            vocab[int(v)] = ""
            failed += 1

    vocab_path = os.path.join(args.out, "vocab.json")
    with open(vocab_path, "w", encoding="utf-8") as f:
        json.dump(vocab, f, ensure_ascii=False)
    print(f"Saved vocab.json: {len(vocab)} entries ({failed} decode failures) → {vocab_path}")

    # ---- Mel filterbank ----
    # The Hailo encoder HEF was compiled from whisper's ONNX which uses
    # librosa.filters.mel(sr=16000, n_fft=400, n_mels=80) with librosa's
    # default O'Shaughnessy mel scale and Slaney normalisation.
    # Saving the exact filterbank avoids any formula mismatch in C++.
    import os as _os
    mel_filters_path = _os.path.join(args.out, "mel_filters_80.npy")
    try:
        assets_dir = _os.path.join(_os.path.dirname(whisper.__file__), "assets")
        mel_npz    = _os.path.join(assets_dir, "mel_filters.npz")
        with np.load(mel_npz) as f:
            mel80 = f["mel_80"]
        np.save(mel_filters_path, mel80)
        print(f"Saved mel filterbank:     {mel80.shape} → {mel_filters_path}")
    except Exception as e:
        print(f"WARNING: could not save mel_filters_80.npy: {e}")
        print("  HailoTranscriber will fall back to computed filterbank.")

    # ---- Summary ----
    d_model = emb.shape[1]
    print(f"\nSummary:")
    print(f"  Model size : {args.model}")
    print(f"  d_model    : {d_model}")
    print(f"  vocab_size : {emb.shape[0]}")
    print(f"  max_ctx    : {pos.shape[0]}")
    print(f"\nVerify that your encoder.hef and decoder.hef were compiled from the")
    print(f"same model size ({args.model}).  If they were compiled from a different")
    print(f"size, rerun with --model <correct_size>.")
    print(f"\nRun velan with:")
    print(f"  ./velan --aicore hailo8 \\")
    print(f"          --encoder-hef {args.out}/encoder.hef \\")
    print(f"          --decoder-hef {args.out}/decoder.hef \\")
    print(f"          --vocab-json  {args.out}/vocab.json")


if __name__ == "__main__":
    main()
