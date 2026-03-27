#!/usr/bin/env python3
"""
leo_kk_demo.py — Leo v4 176M + Knowledge Kernel Demo

Sentence-boundary injection: Leo speaks, KK injects knowledge
at thought boundaries, Leo continues from the injected concept.

The model doesn't copy. It absorbs and reformulates.

Usage:
  PYTHONPATH=/path/to/nanochat python3 leo_kk_demo.py \\
    --weights janus_v4_sft_leo.pt \\
    --tokenizer tokenizer.pkl \\
    --knowledge dario_essay.txt \\
    --concepts "RRPRAM,Echo,Dario Equation,theta"
"""
import pickle, torch, re, argparse, sys
sys.path.insert(0, '/home/ubuntu/nanochat')

BOS, EOS = 32766, 32767


def load_model(weights_path, device='cuda'):
    from janus4.janus.janus_gpt_v4_lowrank import JanusGPT, JanusConfig
    cfg = JanusConfig(vocab_size=32768)
    model = JanusGPT(cfg)
    sd = torch.load(weights_path, map_location='cpu', weights_only=False)
    model.load_state_dict(sd)
    model = model.to(device).to(torch.bfloat16).eval()
    return model


def find_chunk(text, keyword, max_chars=200):
    """Find best paragraph containing keyword."""
    for p in text.split('\n\n'):
        if keyword.lower() in p.lower() and len(p.strip()) > 50:
            c = p.strip()[:max_chars]
            dot = c.rfind('.')
            if dot > 80:
                c = c[:dot + 1]
            return c
    return None


def gen_thought(model, tok, ids, max_tok=80, temp=0.8, top_k=50, rep_pen=1.4):
    """Generate one thought until EOS or repetition."""
    text = []
    recent = []
    for tid in model.generate(ids, max_tokens=max_tok, temperature=temp,
                               top_k=top_k, rep_penalty=rep_pen):
        ids.append(tid)
        recent.append(tid)
        if tid == EOS:
            break
        text.append(tok.decode([tid]))
        # repetition detection
        if len(recent) > 4 and len(set(recent[-4:])) == 1:
            break
    return ''.join(text), ids


def clean_output(text):
    """Remove <|bos|> prefix and <|assistant_end|> suffix."""
    if '<|bos|>' in text:
        text = text.split('<|bos|>')[-1]
    text = text.split('<|assistant_end|>')[0]
    return text.strip()


def inject(ids, concept_text, question, max_context=700):
    """Inject concept at sentence boundary with context trimming."""
    if len(ids) > max_context:
        ids = [BOS] + ids[-(max_context - 1):]
    inject_str = f'\n{concept_text}\nQ: {question}\nA:'
    tok_obj = inject.__dict__.get('_tok')
    if tok_obj:
        ids.extend(tok_obj.encode(inject_str))
    return ids


def run_demo(model, tok, essay, concepts_questions, initial_prompt=None):
    """Run full demo with chain injection."""
    inject._tok = tok  # hack to pass tokenizer

    if initial_prompt is None:
        initial_prompt = 'Q: Tell me about yourself and what you know.\nA:'

    ids = [BOS] + tok.encode(initial_prompt)

    # first thought
    print('[Leo speaks freely]')
    text, ids = gen_thought(model, tok, ids)
    print(clean_output(text))

    # chain injection
    for concept, keyword, question in concepts_questions:
        chunk = find_chunk(essay, keyword)
        if not chunk:
            chunk = find_chunk(essay, concept)
        if not chunk:
            print(f'\n[KK: no chunk found for "{keyword}"]')
            continue

        ids = inject(ids, chunk, question)
        print(f'\n[KK injects {concept}]')
        text, ids = gen_thought(model, tok, ids, max_tok=100)
        print(clean_output(text))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--weights', required=True)
    parser.add_argument('--tokenizer', required=True)
    parser.add_argument('--knowledge', required=True)
    parser.add_argument('--concepts', default='RRPRAM,Echo,Dario Equation,theta')
    parser.add_argument('--device', default='cuda')
    args = parser.parse_args()

    with open(args.tokenizer, 'rb') as f:
        tok = pickle.load(f)

    model = load_model(args.weights, args.device)
    n = sum(p.numel() for p in model.parameters())
    print(f'[model] Janus v4 {n/1e6:.0f}M — Leo voice')

    with open(args.knowledge) as f:
        essay = f.read()
    print(f'[kk] loaded {args.knowledge}: {len(essay)} chars\n')

    # parse concepts into (concept, keyword, question) tuples
    concepts_questions = []
    for c in args.concepts.split(','):
        c = c.strip()
        concepts_questions.append((
            c,
            c,
            f'What does {c} feel like?'
        ))

    print('=' * 50)
    print('  LEO + KK — Resonance Injection')
    print('=' * 50)

    run_demo(model, tok, essay, concepts_questions)

    print('\n' + '=' * 50)


if __name__ == '__main__':
    main()
