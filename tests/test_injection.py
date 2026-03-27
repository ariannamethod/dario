#!/usr/bin/env python3
"""
A/B test: generation WITHOUT vs WITH hidden state injection.
Measures actual word overlap with KK chunk.
"""
import pickle, torch, sys, re, os
sys.path.insert(0, '/home/ubuntu/nanochat')
from nanochat.janus_gpt import JanusGPT, JanusConfig
from resonance_injection import generate_with_injection

# Load
with open('janus4/janus/tokenizer.pkl', 'rb') as f:
    tok = pickle.load(f)

cfg = JanusConfig(vocab_size=32768)
model = JanusGPT(cfg)
sd = torch.load('janus4/janus/sft_22k/janus_177m_v4_sft_leo_22k.pt',
                map_location='cpu', weights_only=False)
model.load_state_dict(sd)
model = model.to('cuda').to(torch.bfloat16).eval()
print('[model] Leo v4 176M loaded\n')

BOS, EOS = 32766, 32767

kk_chunk = """A: By giving each moment its own tune, RRPRAM allows us to discern hidden rhythmic patterns. These transitions, deeply rooted in the conversation, reveal the narrative within. RRPRAM creates fingerprints for each position in the sequence. It assigns a unique weight to every position, forming a positional signature that captures how each moment relates to every other moment."""

def gen_baseline(prompt, seed=42):
    """Generate WITHOUT injection."""
    torch.manual_seed(seed)
    ids = [BOS] + tok.encode(prompt)
    x = torch.tensor([ids], device='cuda')
    out = []
    with torch.no_grad():
        for step in range(150):
            logits = model(x)[:, -1, :].float()[0]
            logits[BOS] = float('-inf')
            for tid in set(ids[-40:]):
                if logits[tid] > 0: logits[tid] /= 1.15
                else: logits[tid] *= 1.15
            logits /= 0.8
            probs = logits.softmax(-1)
            tid = torch.multinomial(probs, 1).item()
            if tid == EOS: break
            out.append(tid)
            x = torch.cat([x, torch.tensor([[tid]], device='cuda')], dim=1)
            ids.append(tid)
    return tok.decode(out)

def gen_injected(prompt, kk_text, seed=42, gate_bias=-1.5):
    """Generate WITH hidden state injection."""
    torch.manual_seed(seed)
    prompt_ids = tok.encode(prompt)
    return generate_with_injection(
        model, tok, prompt_ids, kk_text,
        injection_layer=10, gate_bias=gate_bias,
        max_tokens=150, temperature=0.8, rep_penalty=1.15
    )

def analyze(text_a, text_b, kk_text):
    words_a = set(re.findall(r'[a-zA-Z]{3,}', text_a.lower()))
    words_b = set(re.findall(r'[a-zA-Z]{3,}', text_b.lower()))
    kk_words = set(re.findall(r'[a-zA-Z]{3,}', kk_text.lower()))

    only_b = words_b - words_a
    from_kk = only_b & kk_words

    return {
        'words_a': len(words_a),
        'words_b': len(words_b),
        'only_with_injection': len(only_b),
        'from_kk': from_kk,
        'n_from_kk': len(from_kk),
        'kk_coverage_a': len(words_a & kk_words),
        'kk_coverage_b': len(words_b & kk_words),
    }

prompts = [
    'Q: What is RRPRAM?\nA:',
    'Q: How do patterns emerge in language?\nA:',
    'Q: What makes each position unique?\nA:',
]

print('=' * 70)
print('  A/B TEST: Baseline vs Hidden State Injection')
print('=' * 70)

for prompt in prompts:
    print(f'\n{"─" * 70}')
    print(f'PROMPT: {prompt.strip()}')
    print(f'{"─" * 70}')

    print('\n[A] WITHOUT injection:')
    text_a = gen_baseline(prompt)
    print(text_a[:250])

    print('\n[B] WITH injection:')
    text_b = gen_injected(prompt, kk_chunk, gate_bias=-1.5)

    stats = analyze(text_a, text_b, kk_chunk)
    print(f'\n[STATS]')
    print(f'  Baseline words: {stats["words_a"]}, KK overlap: {stats["kk_coverage_a"]}')
    print(f'  Injected words: {stats["words_b"]}, KK overlap: {stats["kk_coverage_b"]}')
    print(f'  Words ONLY with injection: {stats["only_with_injection"]}')
    print(f'  Of those, FROM KK: {stats["from_kk"]}')
    print(f'  KK coverage delta: +{stats["kk_coverage_b"] - stats["kk_coverage_a"]}')
