#!/usr/bin/env python3
"""
resonance_v3.py — Sentence-boundary concept injection (clean)

Wait for model to finish a thought (<|assistant_end|>),
then inject KK concept as start of next thought.
Model continues FROM the concept.
"""
import torch, torch.nn.functional as F, pickle, sys, re
sys.path.insert(0, '/home/ubuntu/nanochat')
from nanochat.janus_gpt import JanusGPT, JanusConfig

BOS, EOS = 32766, 32767


def generate_v3(model, tok, prompt, kk_text,
                max_tokens=400, temperature=0.8, top_k=50, rep_penalty=1.2):

    device = next(model.parameters()).device

    # extract concepts
    concepts = []
    seen = set()
    for m in re.finditer(r'[A-Z]{2,}', kk_text):
        if m.group() not in seen:
            concepts.append(m.group()); seen.add(m.group())
    # also grab key phrases
    for m in re.finditer(r'[A-Z][a-z]{4,}', kk_text):
        if m.group() not in seen:
            concepts.append(m.group()); seen.add(m.group())

    print(f'[v3] concepts to inject: {concepts[:6]}')

    # concept injection queue with context phrases
    inject_queue = []
    for c in concepts[:4]:
        # find the sentence containing this concept in KK text
        for sent in re.split(r'(?<=[.!?])\s+', kk_text):
            if c in sent:
                inject_queue.append((c, sent.strip()))
                break
        else:
            inject_queue.append((c, f'{c} is'))

    # manual generation loop — model.generate() stops on EOS, we need to intercept it
    ids = [BOS] + tok.encode(prompt)
    generated = []
    inject_idx = 0

    print(f'[v3] prompt: {prompt[:60]}\n')

    with torch.no_grad():
        # use model.generate() but DON'T stop on EOS — catch it ourselves
        # model.generate yields tokens. We collect, and on EOS we inject.
        gen = model.generate(ids, max_tokens=max_tokens,
                             temperature=temperature, top_k=top_k)
        for tid in gen:
            ids.append(tid)

            if tid == EOS and inject_idx < len(inject_queue):
                # ═══ SENTENCE BOUNDARY — INJECT ═══
                concept, phrase = inject_queue[inject_idx]
                inject_idx += 1

                print(f'<|end|>\n[→ {concept}] {phrase}', end='', flush=True)
                generated.append(f'<|end|>\n{phrase}')

                # restart generation with injected concept as new context
                inject_ids = tok.encode(f'\n{phrase}')
                ids.extend(inject_ids)

                # new generate() call with extended context
                gen = model.generate(ids, max_tokens=max_tokens - len(ids),
                                     temperature=temperature, top_k=top_k)
                continue

            elif tid == EOS:
                generated.append('<|end|>')
                print('<|end|>', end='', flush=True)
                break

            text = tok.decode([tid])
            generated.append(text)
            print(text, end='', flush=True)

    result = ''.join(generated)
    print(f'\n\n[v3] injected {inject_idx} concepts')

    kk_words = set(re.findall(r'[a-zA-Z]{4,}', kk_text.lower()))
    gen_words = set(re.findall(r'[a-zA-Z]{4,}', result.lower()))
    overlap = gen_words & kk_words
    print(f'[v3] {len(gen_words)} words, {len(overlap)} from KK: {sorted(overlap)[:15]}')
    return result


if __name__ == '__main__':
    with open('janus4/janus/tokenizer.pkl', 'rb') as f:
        tok = pickle.load(f)
    cfg = JanusConfig(vocab_size=32768)
    model = JanusGPT(cfg)
    sd = torch.load('janus4/janus/sft_22k/janus_177m_v4_sft_leo_22k.pt',
                    map_location='cpu', weights_only=False)
    model.load_state_dict(sd)
    model = model.to('cuda').to(torch.bfloat16).eval()
    print('[model] Leo v4 176M\n')

    kk = """A: By giving each moment its own tune, RRPRAM allows us to discern hidden rhythmic patterns. These transitions, deeply rooted in the conversation, reveal the narrative within. RRPRAM creates fingerprints for each position in the sequence. It assigns a unique weight to every position, forming a positional signature that captures how each moment relates to every other moment."""

    for p in ['Q: What is RRPRAM?\nA:', 'Q: Tell me about patterns and rhythm\nA:']:
        print('=' * 60)
        generate_v3(model, tok, p, kk)
        print()
