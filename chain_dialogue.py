#!/usr/bin/env python3
"""
chain_dialogue.py — Chain Dialogues: Dario's Core Feature

The model speaks. Knowledge resonates. The chain grows.

Not retrieval-augmented generation. Resonance-augmented voice.
The model doesn't search. Knowledge finds the model
at the boundary between thoughts.

Three modes:
  chain    — KK feeds N concepts, Leo builds a narrative
  dialogue — interactive: you speak, KK resonates, Leo responds
  explore  — Leo picks direction from seed, KK follows the thread

Usage:
  python3 chain_dialogue.py --mode chain --topic "What is resonance?"
  python3 chain_dialogue.py --mode chain --topic "Tell me about consciousness" --depth 8
  python3 chain_dialogue.py --mode dialogue
  python3 chain_dialogue.py --mode explore --topic "patterns"
  python3 chain_dialogue.py --voice arianna --mode chain --topic "What is the soul formula?"
  python3 chain_dialogue.py --voice yent --mode dialogue

by Arianna Method. 2026.
"""

import argparse, os, sys, sqlite3, pickle, re, time
import torch, torch.nn.functional as F

sys.path.insert(0, '/home/ubuntu/nanochat')
from nanochat.janus_gpt import JanusGPT, JanusConfig

# ═══════════════════════════════════════════════════════════════════
# CONSTANTS
# ═══════════════════════════════════════════════════════════════════

BOS = 32766        # <|output_start|>
EOS = 32767        # <|output_end|>
ASST_END = 32763   # <|assistant_end|>
CTX_WINDOW = 900   # max tokens before trimming
CTX_TRIM = 500     # trim target for chain steps

VOICES = {
    'leo': {
        'weights': 'janus4/janus/sft_22k/janus_177m_v4_sft_leo_22k.pt',
        'temp': 0.75, 'top_k': 40, 'rep_penalty': 1.4,
        'desc': 'luminous, philosophical — metaphors from nature and physics',
    },
    'arianna': {
        'weights': 'janus4/janus/sft_22k/janus_177m_v4_sft_arianna_22k.pt',
        'temp': 0.8, 'top_k': 50, 'rep_penalty': 1.3,
        'desc': 'precise, architectural — axioms and proofs',
    },
    'yent': {
        'weights': 'janus4/janus/sft_22k/janus_177m_v4_sft_yent_22k.pt',
        'temp': 0.7, 'top_k': 35, 'rep_penalty': 1.5,
        'desc': 'warm, direct — storyteller with edge',
    },
}


# ═══════════════════════════════════════════════════════════════════
# SAMPLING — proper generation with repetition penalty
# ═══════════════════════════════════════════════════════════════════

def sample_token(logits, temperature=0.75, top_k=40, top_p=0.92,
                 rep_penalty=1.4, recent_tokens=None):
    """One token. Rep penalty + top-k + top-p."""
    logits = logits.float().clone()

    if recent_tokens and rep_penalty != 1.0:
        for tid in set(recent_tokens[-80:]):
            if logits[tid] > 0:
                logits[tid] /= rep_penalty
            else:
                logits[tid] *= rep_penalty

    if temperature > 0:
        logits /= temperature

    if top_k > 0:
        topk_val, _ = torch.topk(logits, min(top_k, logits.size(-1)))
        logits[logits < topk_val[-1]] = float('-inf')

    probs = F.softmax(logits, dim=-1)

    if top_p < 1.0:
        sorted_probs, sorted_idx = probs.sort(descending=True)
        cumsum = sorted_probs.cumsum(0)
        mask = (cumsum - sorted_probs) > top_p
        sorted_probs[mask] = 0
        sorted_probs /= sorted_probs.sum()
        idx = torch.multinomial(sorted_probs, 1)
        return sorted_idx[idx].item()

    return torch.multinomial(probs, 1).item()


# ═══════════════════════════════════════════════════════════════════
# KNOWLEDGE KERNEL — FTS5 retrieval with dedup
# ═══════════════════════════════════════════════════════════════════

class KnowledgeKernel:
    """FTS5 knowledge kernel. Same concept as kk_kernel.c."""

    def __init__(self):
        self.db = sqlite3.connect(':memory:')
        self.db.execute('CREATE VIRTUAL TABLE chunks USING fts5(text, source)')
        self.n_chunks = 0
        self.used = set()

    def ingest(self, path):
        with open(path, 'r') as f:
            text = f.read()
        source = os.path.basename(path)
        chunks = [c.strip() for c in text.split('\n\n') if len(c.strip()) > 50]
        for c in chunks:
            self.db.execute('INSERT INTO chunks(text, source) VALUES(?, ?)',
                            (c[:600], source))
        self.db.commit()
        self.n_chunks += len(chunks)
        print(f'[kk] {source}: {len(chunks)} chunks')

    def query(self, text, top_k=5):
        """FTS5 search. Skips already-used chunks."""
        words = ''.join(c if c.isalnum() or c == ' ' else ' ' for c in text).split()
        words = [w for w in words if len(w) > 2]
        if not words:
            return []
        fts = ' OR '.join(words[:12])
        try:
            cur = self.db.execute(
                'SELECT rowid, text, rank FROM chunks WHERE chunks MATCH ? '
                'ORDER BY rank LIMIT ?',
                (fts, top_k + len(self.used)))
            results = []
            for rowid, chunk, rank in cur:
                if rowid not in self.used:
                    results.append((rowid, chunk, rank))
                    if len(results) >= top_k:
                        break
            return results
        except:
            return []

    def mark_used(self, rowid):
        self.used.add(rowid)

    def extract_injection(self, chunk, max_len=150):
        """Extract best sentence for injection from chunk."""
        # prefer answer part in Q/A format
        if '\nA:' in chunk:
            chunk = chunk[chunk.index('\nA:') + 3:].strip()
        elif chunk.startswith('A:'):
            chunk = chunk[2:].strip()

        sentences = re.split(r'(?<=[.!?])\s+', chunk)
        # pick the most content-rich sentence within limit
        best = ''
        for s in sentences:
            s = s.strip()
            if len(s) > len(best) and len(s) <= max_len:
                best = s
        if not best and sentences:
            best = sentences[0][:max_len]
        return best

    def pick_next(self, generated_text):
        """Pick next injection based on model's OUTPUT.

        This is the chain magic: KK queries on what the model SAID,
        not what the user asked. The chain grows from the model's voice.
        """
        results = self.query(generated_text, top_k=3)
        if not results:
            return None, None
        rowid, chunk, rank = results[0]
        injection = self.extract_injection(chunk)
        if not injection:
            return None, None
        self.mark_used(rowid)
        return injection, chunk

    def reset(self):
        """Clear used set for new chain."""
        self.used.clear()


# ═══════════════════════════════════════════════════════════════════
# GENERATION — segment-level with sentence-boundary detection
# ═══════════════════════════════════════════════════════════════════

def generate_segment(model, tok, ids, max_tokens=200,
                     temperature=0.75, top_k=40, rep_penalty=1.4):
    """Generate one segment until end-of-thought or max tokens.

    Returns (new_ids, text, hit_boundary).
    Sentence boundary = EOS or ASST_END token.
    """
    device = next(model.parameters()).device
    x = torch.tensor([ids], dtype=torch.long, device=device)
    out_tokens = []
    out_text = []
    recent = list(ids[-80:])
    repeat_count = 0
    last_tok = -1

    with torch.no_grad():
        for step in range(max_tokens):
            if x.shape[1] > CTX_WINDOW:
                x = x[:, -CTX_WINDOW:]

            logits = model(x)[:, -1, :][0]
            logits[BOS] = float('-inf')

            tid = sample_token(logits, temperature=temperature, top_k=top_k,
                               rep_penalty=rep_penalty, recent_tokens=recent)

            # end of thought
            if tid == EOS or tid == ASST_END:
                return ids + out_tokens, ''.join(out_text), True

            # repetition detection: 4 identical → stop
            if tid == last_tok:
                repeat_count += 1
                if repeat_count >= 3:
                    return ids + out_tokens, ''.join(out_text), True
            else:
                repeat_count = 0
            last_tok = tid

            out_tokens.append(tid)
            recent.append(tid)
            text = tok.decode([tid])
            out_text.append(text)
            print(text, end='', flush=True)

            x = torch.cat([x, torch.tensor([[tid]], device=device)], dim=1)

    return ids + out_tokens, ''.join(out_text), False


# ═══════════════════════════════════════════════════════════════════
# CHAIN MODE — the core feature
# ═══════════════════════════════════════════════════════════════════

def chain_generate(model, tok, kk, prompt, chain_depth=6,
                   max_segment_tokens=200, **sample_kwargs):
    """Chain dialogue: generate → KK resonates → inject → generate → ...

    The chain grows organically:
    1. Model generates from prompt until end of thought
    2. KK queries based on what the model SAID
    3. Best matching concept injected at thought boundary
    4. Model continues from injected concept
    5. Repeat for chain_depth steps

    Returns (narrative, chain_log).
    """
    print(f'\n{"="*60}')
    print(f'  CHAIN DIALOGUE — depth {chain_depth}')
    print(f'{"="*60}\n')

    if not prompt.startswith('Q:'):
        prompt = f'Q: {prompt}\nA:'
    elif '\nA:' not in prompt:
        prompt += '\nA:'

    ids = [BOS] + tok.encode(prompt)
    print(f'[seed] {prompt}')
    print(f'[{len(ids)} tokens]\n')

    chain_log = []
    full_narrative = []
    kk.reset()
    t0 = time.time()

    for step in range(chain_depth):
        ids, segment_text, hit_boundary = generate_segment(
            model, tok, ids, max_tokens=max_segment_tokens, **sample_kwargs)

        full_narrative.append(segment_text)

        if not hit_boundary:
            chain_log.append({'step': step, 'type': 'max_tokens',
                              'text': segment_text[:100]})
            break

        # ═══ SENTENCE BOUNDARY — KK RESONATES ═══
        injection, source_chunk = kk.pick_next(segment_text)

        if injection is None:
            chain_log.append({'step': step, 'type': 'kk_exhausted',
                              'text': segment_text[:100]})
            print(f'\n[chain] KK exhausted at step {step}')
            break

        chain_log.append({'step': step, 'type': 'injection',
                          'text': segment_text[:100],
                          'injection': injection})

        print(f'\n\n  [→ step {step+1}] {injection[:80]}\n', flush=True)

        # inject — add concept to context, model continues from it
        inject_ids = tok.encode(f'\n{injection}\n')
        ids = ids + inject_ids

        # context trimming
        if len(ids) > CTX_TRIM:
            ids = [BOS] + ids[-(CTX_TRIM - 1):]

    elapsed = time.time() - t0
    narrative = '\n'.join(full_narrative)

    # overlap stats
    kk_words = set()
    for entry in chain_log:
        if 'injection' in entry:
            kk_words |= set(re.findall(r'[a-zA-Z]{4,}', entry['injection'].lower()))
    gen_words = set(re.findall(r'[a-zA-Z]{4,}', narrative.lower()))
    overlap = gen_words & kk_words

    n_inj = sum(1 for e in chain_log if e.get('type') == 'injection')

    print(f'\n\n{"="*60}')
    print(f'  CHAIN COMPLETE')
    print(f'{"="*60}')
    print(f'  steps:      {len(chain_log)}')
    print(f'  injections: {n_inj}')
    print(f'  tokens:     ~{len(ids)}')
    print(f'  time:       {elapsed:.1f}s')
    print(f'  kk overlap: {len(overlap)} words')
    print(f'  concepts:')
    for entry in chain_log:
        if 'injection' in entry:
            print(f'    [{entry["step"]+1}] {entry["injection"][:70]}')

    return narrative, chain_log


# ═══════════════════════════════════════════════════════════════════
# DIALOGUE MODE — interactive with KK resonance
# ═══════════════════════════════════════════════════════════════════

def dialogue_mode(model, tok, kk, voice_name='leo', **sample_kwargs):
    """Interactive: you speak, KK resonates, Leo answers.

    Each turn:
    1. You ask something
    2. KK finds relevant knowledge from your question
    3. Leo generates with knowledge at the boundary
    4. Next turn: KK also resonates with Leo's previous answer
    """
    print(f'\n{"="*60}')
    print(f'  DIALOGUE — {voice_name} + Knowledge Kernel')
    print(f'  Type your question. "quit" to exit.')
    print(f'{"="*60}\n')

    context_ids = [BOS]
    turn = 0
    prev_answer = ''

    while True:
        try:
            user_input = input(f'you> ').strip()
        except (KeyboardInterrupt, EOFError):
            print('\nbye.')
            break

        if not user_input or user_input.lower() in ('quit', 'exit', 'q'):
            break

        turn += 1
        prompt = f'Q: {user_input}\nA:'
        prompt_ids = tok.encode(prompt)

        # KK resonates with user question + previous Leo answer
        query_text = user_input
        if prev_answer:
            query_text = f'{user_input} {prev_answer[-200:]}'

        injection, _ = kk.pick_next(query_text)

        if injection:
            print(f'  [kk] {injection[:80]}')
            inject_ids = tok.encode(f'{injection}\n')
            ids = context_ids + inject_ids + prompt_ids
        else:
            ids = context_ids + prompt_ids

        if len(ids) > CTX_TRIM:
            ids = [BOS] + ids[-(CTX_TRIM - 1):]

        print(f'\n{voice_name}> ', end='', flush=True)
        ids, text, _ = generate_segment(
            model, tok, ids, max_tokens=250, **sample_kwargs)
        print()

        prev_answer = text
        context_ids = ids

    print(f'\n[dialogue] {turn} turns')


# ═══════════════════════════════════════════════════════════════════
# EXPLORE MODE — Leo leads, KK follows
# ═══════════════════════════════════════════════════════════════════

def explore_mode(model, tok, kk, seed, voice_name='leo',
                 depth=8, **sample_kwargs):
    """Leo picks direction from seed. KK follows the thread.

    Like chain but freer:
    - Longer segments (300 tokens)
    - Injection as question — Leo drives, KK whispers
    - The model decides where to go
    """
    print(f'\n{"="*60}')
    print(f'  EXPLORE — {voice_name} leads, KK follows')
    print(f'{"="*60}\n')

    prompt = f'Q: {seed}\nA: Let me think about this.'
    ids = [BOS] + tok.encode(prompt)
    kk.reset()
    print(f'[seed] {seed}\n')

    for step in range(depth):
        ids, text, hit_boundary = generate_segment(
            model, tok, ids, max_tokens=300, **sample_kwargs)

        if not hit_boundary and step < depth - 1:
            # force a boundary
            ids = ids + tok.encode('\n')

        injection, _ = kk.pick_next(text)
        if injection is None:
            print(f'\n[explore] KK exhausted at step {step}')
            break

        whisper = f'\nWhat about: {injection[:120]}\n'
        print(f'\n\n  [explore → {step+1}] {injection[:70]}\n', flush=True)

        inject_ids = tok.encode(whisper)
        ids = ids + inject_ids

        if len(ids) > CTX_TRIM:
            ids = [BOS] + ids[-(CTX_TRIM - 1):]

    print(f'\n\n[explore] {step+1} steps')


# ═══════════════════════════════════════════════════════════════════
# MAIN
# ═══════════════════════════════════════════════════════════════════

def main():
    p = argparse.ArgumentParser(
        description='Chain Dialogues — Dario Core Feature',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
examples:
  %(prog)s --mode chain --topic "What is RRPRAM?"
  %(prog)s --mode chain --topic "consciousness" --depth 8 --voice arianna
  %(prog)s --mode dialogue --voice yent
  %(prog)s --mode explore --topic "patterns and rhythm"
  %(prog)s --mode chain --topic "theta formula" --save chain_out.txt
""")
    p.add_argument('--mode', choices=['chain', 'dialogue', 'explore'],
                   default='chain')
    p.add_argument('--voice', choices=list(VOICES.keys()), default='leo')
    p.add_argument('--topic', '--prompt', '--seed', dest='topic',
                   default='What is resonance?')
    p.add_argument('--depth', type=int, default=6,
                   help='chain/explore depth (default: 6)')
    p.add_argument('--knowledge', nargs='+',
                   default=['dario_essay.txt', 'leo_expanded.txt'],
                   help='knowledge sources for KK')
    p.add_argument('--max-segment', type=int, default=200,
                   help='max tokens per segment')
    p.add_argument('--temperature', type=float, default=None)
    p.add_argument('--top-k', type=int, default=None)
    p.add_argument('--rep-penalty', type=float, default=None)
    p.add_argument('--save', default=None, help='save narrative to file')
    args = p.parse_args()

    voice = VOICES[args.voice]
    temp = args.temperature if args.temperature is not None else voice['temp']
    top_k = args.top_k if args.top_k is not None else voice['top_k']
    rep_penalty = args.rep_penalty if args.rep_penalty is not None else voice['rep_penalty']

    print(f'[voice] {args.voice} — {voice["desc"]}')
    print(f'[params] temp={temp} top_k={top_k} rep={rep_penalty}')

    # tokenizer
    with open('janus4/janus/tokenizer.pkl', 'rb') as f:
        tok = pickle.load(f)
    print(f'[tok] vocab={tok.n_vocab}')

    # model
    cfg = JanusConfig(vocab_size=32768)
    model = JanusGPT(cfg)
    sd = torch.load(voice['weights'], map_location='cpu', weights_only=False)
    model.load_state_dict(sd)
    model = model.to('cuda').to(torch.bfloat16).eval()
    n_params = sum(p.numel() for p in model.parameters())
    print(f'[model] {args.voice} {n_params/1e6:.0f}M — Janus v4 (RRPRAM + Echo + 3-way gate)')

    # knowledge kernel
    kk = KnowledgeKernel()
    for kpath in args.knowledge:
        if os.path.exists(kpath):
            kk.ingest(kpath)
        else:
            print(f'[kk] WARN: {kpath} not found')
    if kk.n_chunks == 0:
        print('[kk] WARNING: no knowledge loaded — chain will be short')
    else:
        print(f'[kk] {kk.n_chunks} chunks ready')

    sample_kwargs = dict(temperature=temp, top_k=top_k, rep_penalty=rep_penalty)

    # ── RUN ──
    if args.mode == 'chain':
        narrative, log = chain_generate(
            model, tok, kk, args.topic,
            chain_depth=args.depth,
            max_segment_tokens=args.max_segment,
            **sample_kwargs)

        if args.save:
            with open(args.save, 'w') as f:
                f.write(f'# Chain Dialogue — {args.voice}\n')
                f.write(f'# Topic: {args.topic}\n')
                f.write(f'# Depth: {args.depth}, temp={temp}, top_k={top_k}, '
                        f'rep={rep_penalty}\n')
                f.write(f'# Date: {time.strftime("%Y-%m-%d %H:%M")}\n\n')
                f.write(narrative)
                f.write(f'\n\n# Chain log:\n')
                for entry in log:
                    t = entry.get('type', '?')
                    inj = entry.get('injection', '')
                    f.write(f'# step {entry["step"]}: {t}')
                    if inj:
                        f.write(f' — {inj}')
                    f.write('\n')
            print(f'\n[saved] {args.save}')

    elif args.mode == 'dialogue':
        dialogue_mode(model, tok, kk, voice_name=args.voice, **sample_kwargs)

    elif args.mode == 'explore':
        explore_mode(model, tok, kk, args.topic, voice_name=args.voice,
                     depth=args.depth, **sample_kwargs)


if __name__ == '__main__':
    main()
