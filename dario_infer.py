#!/usr/bin/env python3
"""
dario_infer.py — Python wrapper for notorch C inference.

Loads tiktoken tokenizer, encodes prompts, calls infer_v4 (C + notorch BLAS),
decodes output. No PyTorch. No torch.load. Just tiktoken + subprocess + C.

Usage:
    python3 dario_infer.py --voice leo "What is resonance?"
    python3 dario_infer.py --voice arianna --temp 0.5 "Tell me about the Method"
    python3 dario_infer.py --voice yent --max 100 "Who are you?"
"""

import os
import sys
import struct
import subprocess
import tempfile
import pickle
import argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# Voice → weight file mapping
VOICES = {
    'leo':     'janus_v4_sft_leo_notorch.bin',
    'arianna': 'janus_v4_sft_arianna_notorch.bin',
    'yent':    'janus_v4_sft_yent_notorch.bin',
    'base':    'janus_v4_base_22k.bin',
}

# Search paths for weights
WEIGHT_DIRS = [
    os.path.join(SCRIPT_DIR, 'weights'),
    os.path.expanduser('~/janus-v4-final'),
    SCRIPT_DIR,
]

# Tokenizer
TOK_PATHS = [
    os.path.join(SCRIPT_DIR, 'tokenizer.pkl'),
    os.path.expanduser('~/janus-v4-weights/tokenizer.pkl'),
]


def find_file(name, dirs):
    for d in dirs:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None


def load_tokenizer():
    for p in TOK_PATHS:
        if os.path.exists(p):
            with open(p, 'rb') as f:
                return pickle.load(f)
    print("ERROR: tokenizer.pkl not found", file=sys.stderr)
    sys.exit(1)


def build_infer_v4():
    """Build infer_v4 if not exists."""
    binary = os.path.join(SCRIPT_DIR, 'infer_v4')
    if os.path.exists(binary):
        return binary
    print("Building infer_v4 with notorch BLAS...", file=sys.stderr)
    subprocess.run(['make', 'infer_v4'], cwd=SCRIPT_DIR, check=True,
                   capture_output=True)
    return binary


def infer(voice, prompt, max_tokens=80, temp=0.6):
    """Run inference: encode → C forward → decode."""
    tok = load_tokenizer()
    binary = build_infer_v4()

    # Find weights
    weight_file = VOICES.get(voice)
    if not weight_file:
        print(f"Unknown voice: {voice}. Available: {list(VOICES.keys())}", file=sys.stderr)
        sys.exit(1)
    weight_path = find_file(weight_file, WEIGHT_DIRS)
    if not weight_path:
        print(f"Weights not found: {weight_file}", file=sys.stderr)
        sys.exit(1)

    # Encode prompt
    full_prompt = f"Q: {prompt}\nA:"
    ids = tok.encode(full_prompt)

    # Write tokens to temp file
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(struct.pack('<i', len(ids)))
        for t in ids:
            f.write(struct.pack('<i', t))
        tok_path = f.name

    try:
        # Run C inference
        result = subprocess.run(
            [binary, weight_path, tok_path, str(max_tokens), str(temp)],
            capture_output=True, text=True, timeout=120
        )

        # Extract generation from output
        output = result.stdout
        if '--- generation ---' in output:
            gen_text = output.split('--- generation ---\n', 1)[1]
            # Remove the stats line at the end
            lines = gen_text.strip().split('\n')
            if lines and lines[-1].startswith('[janus'):
                stats = lines[-1]
                gen_text = '\n'.join(lines[:-1])
            else:
                stats = ''

            return gen_text.strip(), stats
        else:
            return output, ''
    finally:
        os.unlink(tok_path)


def main():
    parser = argparse.ArgumentParser(description='Janus v4 inference via notorch')
    parser.add_argument('prompt', nargs='?', default='What is resonance?')
    parser.add_argument('--voice', '-v', default='leo', choices=list(VOICES.keys()))
    parser.add_argument('--max', '-n', type=int, default=80)
    parser.add_argument('--temp', '-t', type=float, default=0.6)
    args = parser.parse_args()

    text, stats = infer(args.voice, args.prompt, args.max, args.temp)
    print(f"\n[{args.voice}] {text}")
    if stats:
        print(f"\n{stats}")


if __name__ == '__main__':
    main()
