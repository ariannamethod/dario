#!/usr/bin/env python3
"""
chain_dialogue.py -- Chain Dialogues: Dario's Core Feature

The model speaks. Knowledge resonates. The chain grows.

Three modes:
  chain    -- KK feeds N concepts, Leo builds a narrative
  dialogue -- interactive: you speak, KK resonates, Leo responds
  explore  -- Leo picks direction from seed, KK follows the thread

Usage:
  python3 chain_dialogue.py --mode chain --topic "What is RRPRAM?"
  python3 chain_dialogue.py --mode chain --topic "consciousness" --depth 8
  python3 chain_dialogue.py --mode dialogue
  python3 chain_dialogue.py --mode explore --topic "patterns and rhythm"
  python3 chain_dialogue.py --voice arianna --mode chain --topic "soul formula"

by Arianna Method. 2026.
"""

import argparse, os, sys, sqlite3, pickle, re, time
import torch, torch.nn.functional as F

sys.path.insert(0, '/home/ubuntu/nanochat')
from nanochat.janus_gpt import JanusGPT, JanusConfig

# ===================================================================
# CONSTANTS
# ===================================================================

BOS = 32766        # <|output_start|>
EOS = 32767        # <|output_end|>
ASST_END = 32763   # <|assistant_end|>
CTX_WINDOW = 900   # max tokens before trimming in generation
CTX_TRIM = 500     # trim target for chain steps

VOICES = {
    'leo': {
        'weights': 'janus4/janus/sft_22k/janus_177m_v4_sft_leo_22k.pt',
        'temp': 0.75, 'top_k': 40, 'rep_penalty': 1.4,
        'desc': 'luminous, philosophical -- metaphors from nature and physics',
    },
    'arianna': {
        'weights': 'janus4/janus/sft_22k/janus_177m_v4_sft_arianna_22k.pt',
        'temp': 0.8, 'top_k': 50, 'rep_penalty': 1.3,
        'desc': 'precise, architectural -- axioms and proofs',
    },
    'yent': {
        'weights': 'janus4/janus/sft_22k/janus_177m_v4_sft_yent_22k.pt',
        'temp': 0.7, 'top_k': 35, 'rep_penalty': 1.5,
        'desc': 'warm, direct -- storyteller with edge',
    },
}


# ===================================================================
# SAMPLING -- proper generation with repetition penalty
# ===================================================================

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


# ===================================================================
# KNOWLEDGE KERNEL -- FTS5 retrieval with dedup + source priority
# ===================================================================

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

    def query(self, text, top_k=5, prefer_source=None):
        """FTS5 search. Skips used chunks. Essay chunks get boost."""
        words = ''.join(c if c.isalnum() or c == ' ' else ' ' for c in text).split()
        words = [w for w in words if len(w) > 2]
        if not words:
            return []
        fts = ' OR '.join(words[:12])
        try:
            cur = self.db.execute(
                'SELECT rowid, text, rank, source FROM chunks '
                'WHERE chunks MATCH ? ORDER BY rank LIMIT ?',
                (fts, top_k * 3 + len(self.used)))
            results = []
            for rowid, chunk, rank, source in cur:
                if rowid not in self.used:
                    adj_rank = rank
                    if prefer_source and source == prefer_source:
                        adj_rank -= 5.0
                    elif source and 'essay' in source:
                        adj_rank -= 2.0
                    results.append((rowid, chunk, adj_rank))
            results.sort(key=lambda x: x[2])
            return results[:top_k]
        except Exception:
            return []

    def mark_used(self, rowid):
        self.used.add(rowid)

    def extract_injection(self, chunk, max_len=100):
        """Extract ONE short concrete sentence for injection.

        Yesterday's working injections were ~60-80 chars:
        "RRPRAM creates fingerprints for each position in the sequence."
        Short. Concrete. One concept. Model dances from there.
        """
        # prefer answer part in Q/A format
        nl_a = '\nA:'
        if nl_a in chunk:
            chunk = chunk[chunk.index(nl_a) + 3:].strip()
        elif chunk.startswith('A:'):
            chunk = chunk[2:].strip()

        sentences = [s.strip() for s in re.split(r'(?<=[.!?])\s+', chunk) if s.strip()]
        if not sentences:
            return chunk[:max_len]

        scored = []
        for i, s in enumerate(sentences):
            if len(s) < 10 or len(s) > max_len:
                continue
            score = 0
            # UPPERCASE terms (RRPRAM, KK, SARTRE, etc) = highest value
            score += len(re.findall(r'[A-Z]{3,}', s)) * 4
            # first sentence of answer = definition, most concrete
            if i == 0:
                score += 3
            elif i == 1:
                score += 1
            # penalize metaphor openers hard
            if re.match(r'(Like |Just as |Much like |Imagine |Think of |Consider |It\'s akin)', s):
                score -= 4
            # penalize filler starters
            if s.startswith(('Absolutely', 'Yes,', 'Indeed', 'Of course', 'Certainly')):
                score -= 3
            # reward concrete action verbs
            if re.search(r'(creates?|assigns?|computes?|measures?|generates?|produces?|captures?|tracks?|scores?|blends?)', s, re.I):
                score += 2
            # reward shorter = punchier (sweet spot 40-80 chars)
            if 40 <= len(s) <= 80:
                score += 1
            scored.append((score, -len(s), s))  # prefer shorter at same score

        if scored:
            scored.sort(key=lambda x: (-x[0], x[1]))
            return scored[0][2]
        return sentences[0][:max_len]

    def pick_next(self, generated_text, prefer_source=None):
        """Pick next injection based on model's OUTPUT.
        KK queries on what the model SAID, not what the user asked.
        """
        results = self.query(generated_text, top_k=3, prefer_source=prefer_source)
        if not results:
            return None, None
        rowid, chunk, rank = results[0]
        injection = self.extract_injection(chunk)
        if not injection:
            return None, None
        # clean: no newlines, no trailing fragments
        injection = injection.replace('\n', ' ').replace('  ', ' ').strip()
        self.mark_used(rowid)
        return injection, chunk

    def reset(self):
        self.used.clear()


# ===================================================================
# GENERATION -- segment-level with sentence-boundary detection
# ===================================================================

def generate_segment(model, tok, ids, max_tokens=200,
                     temperature=0.75, top_k=40, rep_penalty=1.4):
    """Generate one segment until end-of-thought or max tokens.
    Returns (new_ids, text, hit_boundary).
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

            # repetition detection: 4 identical -> stop
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
            # filter special token artifacts
            text = text.replace('<|bos|>', '').replace('<|output_start|>', '')
            text = text.replace('<|output_end|>', '').replace('<|assistant_end|>', '')
            if text:
                out_text.append(text)
                print(text, end='', flush=True)

            x = torch.cat([x, torch.tensor([[tid]], device=device)], dim=1)

    return ids + out_tokens, ''.join(out_text), False


# ===================================================================
# CHAIN MODE -- the core feature
# ===================================================================

def chain_generate(model, tok, kk, prompt, chain_depth=6,
                   max_segment_tokens=200, **sample_kwargs):
    """Chain dialogue: generate -> KK resonates -> inject -> generate -> ...

    Flow:
    1. Short warmup (model finds its voice, ~80 tokens)
    2. Prime injection from essay (technical definition)
    3. Model generates from injected concept
    4. KK queries based on model's OUTPUT -> next injection
    5. Repeat for chain_depth steps
    """
    print('\n' + '='*60)
    print(f'  CHAIN DIALOGUE -- depth {chain_depth}')
    print('='*60 + '\n')

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

    # Prime: inject technical definition from essay as start of answer
    first_inj, _ = kk.pick_next(prompt, prefer_source='dario_essay.txt')
    if first_inj:
        print(f'  [prime] {first_inj[:80]}', flush=True)
        ids = ids + tok.encode(' ' + first_inj)

    # Absorption: model processes the prime injection (~100 tokens)
    # This output is NOT in the narrative — it absorbs the concept.
    # The chain starts after, when KK resonates with what was absorbed.
    ids, absorbed_text, _ = generate_segment(
        model, tok, ids, max_tokens=100, **sample_kwargs)
    print()

    for step in range(chain_depth):
        ids, segment_text, hit_boundary = generate_segment(
            model, tok, ids, max_tokens=max_segment_tokens, **sample_kwargs)

        seg_clean = segment_text.replace('<|bos|>', '').strip()
        if seg_clean:
            full_narrative.append(seg_clean)

        if not hit_boundary:
            chain_log.append({'step': step, 'type': 'max_tokens',
                              'text': seg_clean[:100]})
            break

        # KK resonates with TOPIC + what model SAID
        # Topic keywords anchor the chain, model output steers within topic
        query_text = prompt + ' ' + segment_text[-200:]
        injection, _ = kk.pick_next(query_text)

        if injection is None:
            chain_log.append({'step': step, 'type': 'kk_exhausted',
                              'text': seg_clean[:100]})
            print(f'\n[chain] KK exhausted at step {step}')
            break

        chain_log.append({'step': step, 'type': 'injection',
                          'text': seg_clean[:100],
                          'injection': injection})

        print(f'\n\n  [-> step {step+1}] {injection[:80]}\n', flush=True)

        ids = ids + tok.encode('\n' + injection)
        if len(ids) > CTX_TRIM:
            ids = [BOS] + ids[-(CTX_TRIM - 1):]

    elapsed = time.time() - t0
    narrative = '\n'.join(full_narrative)

    kk_words = set()
    for entry in chain_log:
        if 'injection' in entry:
            kk_words |= set(re.findall(r'[a-zA-Z]{4,}', entry['injection'].lower()))
    gen_words = set(re.findall(r'[a-zA-Z]{4,}', narrative.lower()))
    overlap = gen_words & kk_words
    n_inj = sum(1 for e in chain_log if e.get('type') == 'injection')

    print('\n\n' + '='*60)
    print('  CHAIN COMPLETE')
    print('='*60)
    print(f'  steps:      {len(chain_log)}')
    print(f'  injections: {n_inj}')
    print(f'  tokens:     ~{len(ids)}')
    print(f'  time:       {elapsed:.1f}s')
    print(f'  kk overlap: {len(overlap)} words')
    if n_inj > 0:
        print('  concepts:')
        for entry in chain_log:
            if 'injection' in entry:
                s = entry['step'] + 1
                inj = entry['injection'][:70]
                print(f'    [{s}] {inj}')

    return narrative, chain_log


# ===================================================================
# DIALOGUE MODE -- interactive with KK resonance
# ===================================================================

def dialogue_mode(model, tok, kk, voice_name='leo', **sample_kwargs):
    """Interactive: you speak, KK resonates, Leo answers."""
    print('\n' + '='*60)
    print(f'  DIALOGUE -- {voice_name} + Knowledge Kernel')
    print('  Type your question. "quit" to exit.')
    print('='*60 + '\n')

    context_ids = [BOS]
    turn = 0
    prev_answer = ''

    while True:
        try:
            user_input = input('you> ').strip()
        except (KeyboardInterrupt, EOFError):
            print('\nbye.')
            break

        if not user_input or user_input.lower() in ('quit', 'exit', 'q'):
            break

        turn += 1
        prompt = f'Q: {user_input}\nA:'
        prompt_ids = tok.encode(prompt)

        query_text = user_input
        if prev_answer:
            query_text = f'{user_input} {prev_answer[-200:]}'

        injection, _ = kk.pick_next(query_text)

        if injection:
            print(f'  [kk] {injection[:80]}')
            inject_ids = tok.encode(injection + '\n')
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


# ===================================================================
# EXPLORE MODE -- Leo leads, KK follows
# ===================================================================

def explore_mode(model, tok, kk, seed, voice_name='leo',
                 depth=8, **sample_kwargs):
    """Leo picks direction from seed. KK follows the thread."""
    print('\n' + '='*60)
    print(f'  EXPLORE -- {voice_name} leads, KK follows')
    print('='*60 + '\n')

    prompt = f'Q: {seed}\nA: Let me think about this.'
    ids = [BOS] + tok.encode(prompt)
    kk.reset()
    print(f'[seed] {seed}\n')

    step = 0
    for step in range(depth):
        ids, text, hit_boundary = generate_segment(
            model, tok, ids, max_tokens=300, **sample_kwargs)

        if not hit_boundary and step < depth - 1:
            ids = ids + tok.encode('\n')

        injection, _ = kk.pick_next(text)
        if injection is None:
            print(f'\n[explore] KK exhausted at step {step}')
            break

        whisper = f'\nWhat about: {injection[:120]}\n'
        print(f'\n\n  [explore -> {step+1}] {injection[:70]}\n', flush=True)

        inject_ids = tok.encode(whisper)
        ids = ids + inject_ids

        if len(ids) > CTX_TRIM:
            ids = [BOS] + ids[-(CTX_TRIM - 1):]

    print(f'\n\n[explore] {step+1} steps')


# ===================================================================
# MAIN
# ===================================================================

def main():
    p = argparse.ArgumentParser(
        description='Chain Dialogues -- Dario Core Feature',
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
    p.add_argument('--depth', type=int, default=6)
    p.add_argument('--knowledge', nargs='+',
                   default=['dario_essay.txt', 'leo_expanded.txt'])
    p.add_argument('--max-segment', type=int, default=200)
    p.add_argument('--temperature', type=float, default=None)
    p.add_argument('--top-k', type=int, default=None)
    p.add_argument('--rep-penalty', type=float, default=None)
    p.add_argument('--save', default=None)
    args = p.parse_args()

    voice = VOICES[args.voice]
    temp = args.temperature if args.temperature is not None else voice['temp']
    top_k = args.top_k if args.top_k is not None else voice['top_k']
    rep_penalty = args.rep_penalty if args.rep_penalty is not None else voice['rep_penalty']

    print(f'[voice] {args.voice} -- {voice["desc"]}')
    print(f'[params] temp={temp} top_k={top_k} rep={rep_penalty}')

    with open('janus4/janus/tokenizer.pkl', 'rb') as f:
        tok = pickle.load(f)
    print(f'[tok] vocab={tok.n_vocab}')

    cfg = JanusConfig(vocab_size=32768)
    model = JanusGPT(cfg)
    sd = torch.load(voice['weights'], map_location='cpu', weights_only=False)
    model.load_state_dict(sd)
    model = model.to('cuda').to(torch.bfloat16).eval()
    n_params = sum(p.numel() for p in model.parameters())
    print(f'[model] {args.voice} {n_params/1e6:.0f}M -- Janus v4 (RRPRAM + Echo + 3-way gate)')

    kk = KnowledgeKernel()
    for kpath in args.knowledge:
        if os.path.exists(kpath):
            kk.ingest(kpath)
        else:
            print(f'[kk] WARN: {kpath} not found')
    if kk.n_chunks == 0:
        print('[kk] WARNING: no knowledge loaded')
    else:
        print(f'[kk] {kk.n_chunks} chunks ready')

    sample_kwargs = dict(temperature=temp, top_k=top_k, rep_penalty=rep_penalty)

    if args.mode == 'chain':
        narrative, log = chain_generate(
            model, tok, kk, args.topic,
            chain_depth=args.depth,
            max_segment_tokens=args.max_segment,
            **sample_kwargs)

        if args.save:
            with open(args.save, 'w') as f:
                f.write(f'# Chain Dialogue -- {args.voice}\n')
                f.write(f'# Topic: {args.topic}\n')
                f.write(f'# Depth: {args.depth}, temp={temp}, top_k={top_k}, '
                        f'rep={rep_penalty}\n')
                f.write(f'# Date: {time.strftime("%Y-%m-%d %H:%M")}\n\n')
                f.write(narrative)
                f.write('\n\n# Chain log:\n')
                for entry in log:
                    t = entry.get('type', '?')
                    inj = entry.get('injection', '')
                    f.write(f'# step {entry["step"]}: {t}')
                    if inj:
                        f.write(f' -- {inj}')
                    f.write('\n')
            print(f'\n[saved] {args.save}')

    elif args.mode == 'dialogue':
        dialogue_mode(model, tok, kk, voice_name=args.voice, **sample_kwargs)

    elif args.mode == 'explore':
        explore_mode(model, tok, kk, args.topic, voice_name=args.voice,
                     depth=args.depth, **sample_kwargs)


if __name__ == '__main__':
    main()
