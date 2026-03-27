#!/usr/bin/env python3
"""
dario_v4.py — Leo v4 176M + Knowledge Kernel

Janus architecture: RRPRAM + Echo + 3-way gate, 176M params.
Knowledge Kernel: FTS5 retrieval → context injection.
The model doesn't search — knowledge resonates.

Usage:
  python3 dario_v4.py --weights sft_leo.pt --knowledge leo_expanded.txt
  python3 dario_v4.py --weights sft_leo.pt --knowledge leo_expanded.txt --prompt "What is RRPRAM?"
  python3 dario_v4.py --weights sft_leo.pt  # no KK, pure generation

Requires: nanochat in PYTHONPATH, tiktoken
"""
import argparse, os, sys, sqlite3, pickle, torch, torch.nn.functional as F

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'nanochat'))
sys.path.insert(0, '/home/ubuntu/nanochat')

from nanochat.janus_gpt import JanusGPT, JanusConfig

BOS = 32766   # <|output_start|>
EOS = 32767   # <|output_end|>

# ═══════════════════════════════════════════════════════════════════
# SAMPLING — proper generation with repetition penalty
# ═══════════════════════════════════════════════════════════════════

def sample_token(logits, temperature=0.8, top_k=50, top_p=0.92,
                 rep_penalty=1.15, recent_tokens=None):
    """Sample next token with rep penalty + top-k + top-p."""
    logits = logits.float().clone()

    # repetition penalty on recent tokens
    if recent_tokens and rep_penalty != 1.0:
        for tid in set(recent_tokens[-60:]):
            if logits[tid] > 0:
                logits[tid] /= rep_penalty
            else:
                logits[tid] *= rep_penalty

    # temperature
    if temperature > 0:
        logits /= temperature

    # top-k
    if top_k > 0:
        topk_val, _ = torch.topk(logits, min(top_k, logits.size(-1)))
        logits[logits < topk_val[-1]] = float('-inf')

    probs = F.softmax(logits, dim=-1)

    # top-p (nucleus)
    if top_p < 1.0:
        sorted_probs, sorted_idx = probs.sort(descending=True)
        cumsum = sorted_probs.cumsum(0)
        mask = (cumsum - sorted_probs) > top_p
        sorted_probs[mask] = 0
        sorted_probs /= sorted_probs.sum()
        idx = torch.multinomial(sorted_probs, 1)
        return sorted_idx[idx].item()

    return torch.multinomial(probs, 1).item()


def generate(model, tok, prompt_ids, max_tokens=300,
             kk_boost_ids=None, kk_boost_strength=0.0, **sample_kwargs):
    """Generate with optional KK logit boost on knowledge tokens."""
    device = next(model.parameters()).device
    ids = torch.tensor([prompt_ids], dtype=torch.long, device=device)
    generated = list(prompt_ids)
    out_tokens = []

    with torch.no_grad():
        for step in range(max_tokens):
            x = ids[:, -1024:]
            logits = model(x)[:, -1, :]  # [1, V]
            logits = logits[0]  # [V]

            # KK logit boost — soft bias toward knowledge tokens
            if kk_boost_ids and kk_boost_strength > 0:
                for tid in kk_boost_ids:
                    if 0 <= tid < logits.size(0):
                        logits[tid] += kk_boost_strength

            # ban BOS from being generated
            logits[BOS] = float('-inf')

            tid = sample_token(logits, recent_tokens=generated, **sample_kwargs)
            if tid == EOS:
                break

            generated.append(tid)
            out_tokens.append(tid)
            ids = torch.cat([ids, torch.tensor([[tid]], device=device)], dim=1)

            text = tok.decode([tid])
            print(text, end='', flush=True)

    return out_tokens


# ═══════════════════════════════════════════════════════════════════
# KNOWLEDGE KERNEL — FTS5 retrieval + context injection
# ═══════════════════════════════════════════════════════════════════

class KnowledgeKernel:
    """Simple FTS5-based KK. Same concept as kk_kernel.c."""

    def __init__(self, path=None):
        self.db = sqlite3.connect(':memory:')
        self.db.execute('CREATE VIRTUAL TABLE chunks USING fts5(text)')
        self.n_chunks = 0
        if path:
            self.ingest(path)

    def ingest(self, path):
        with open(path, 'r') as f:
            text = f.read()
        chunks = [c.strip() for c in text.split('\n\n') if len(c.strip()) > 50]
        for c in chunks:
            self.db.execute('INSERT INTO chunks(text) VALUES(?)', (c[:600],))
        self.db.commit()
        self.n_chunks = len(chunks)
        print(f'[kk] ingested {path}: {self.n_chunks} chunks')

    def query(self, text, top_k=3):
        words = ''.join(c if c.isalnum() or c == ' ' else ' ' for c in text).split()
        if not words:
            return []
        fts = ' OR '.join(w for w in words[:10] if len(w) > 2)
        if not fts:
            return []
        try:
            cur = self.db.execute(
                'SELECT text, rank FROM chunks WHERE chunks MATCH ? ORDER BY rank LIMIT ?',
                (fts, top_k))
            return cur.fetchall()
        except:
            return []

    def get_context(self, prompt, max_chars=500):
        """Find relevant chunk and extract answer for context injection."""
        results = self.query(prompt, top_k=2)
        if not results:
            return None

        chunk = results[0][0]
        # extract answer part from Q/A format
        if '\nA:' in chunk:
            answer = chunk[chunk.index('\nA:') + 1:]
        elif 'A: ' in chunk:
            answer = chunk[chunk.index('A: '):]
        else:
            answer = chunk

        if len(answer) > max_chars:
            # cut at sentence boundary
            cut = answer[:max_chars].rfind('.')
            if cut > 100:
                answer = answer[:cut + 1]
            else:
                answer = answer[:max_chars]

        return answer.strip()


# ═══════════════════════════════════════════════════════════════════
# MAIN
# ═══════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description='Leo v4 176M + Knowledge Kernel')
    parser.add_argument('--weights', required=True, help='Path to .pt checkpoint')
    parser.add_argument('--tokenizer', default=None, help='Path to tokenizer.pkl')
    parser.add_argument('--knowledge', default=None, help='Text file to ingest into KK')
    parser.add_argument('--prompt', default=None, help='Single prompt (non-interactive)')
    parser.add_argument('--temperature', type=float, default=0.8)
    parser.add_argument('--top-k', type=int, default=50)
    parser.add_argument('--top-p', type=float, default=0.92)
    parser.add_argument('--rep-penalty', type=float, default=1.15)
    parser.add_argument('--max-tokens', type=int, default=300)
    parser.add_argument('--no-kk', action='store_true', help='Disable KK even if --knowledge given')
    args = parser.parse_args()

    # find tokenizer
    tok_path = args.tokenizer
    if not tok_path:
        # look next to weights
        d = os.path.dirname(args.weights)
        for candidate in [os.path.join(d, 'tokenizer.pkl'),
                          os.path.join(d, '..', 'tokenizer.pkl'),
                          'janus4/janus/tokenizer.pkl',
                          'tokenizer.pkl']:
            if os.path.exists(candidate):
                tok_path = candidate
                break
    if not tok_path:
        print('ERROR: tokenizer.pkl not found')
        sys.exit(1)

    with open(tok_path, 'rb') as f:
        tok = pickle.load(f)
    print(f'[tok] loaded {tok_path} (vocab={tok.n_vocab})')

    # load model
    cfg = JanusConfig(vocab_size=32768)
    model = JanusGPT(cfg)
    sd = torch.load(args.weights, map_location='cpu', weights_only=False)
    model.load_state_dict(sd)
    model = model.to('cuda').to(torch.bfloat16).eval()
    n_params = sum(p.numel() for p in model.parameters())
    print(f'[model] Janus v4 {n_params/1e6:.0f}M — RRPRAM + Echo + 3-way gate')

    # knowledge kernel
    kk = None
    if args.knowledge and not args.no_kk:
        kk = KnowledgeKernel(args.knowledge)

    sample_kwargs = dict(
        temperature=args.temperature,
        top_k=args.top_k,
        top_p=args.top_p,
        rep_penalty=args.rep_penalty,
    )

    def run_prompt(prompt_text):
        # format as Q/A
        if not prompt_text.startswith('Q:'):
            prompt_text = f'Q: {prompt_text}\nA:'
        elif '\nA:' not in prompt_text:
            prompt_text = prompt_text + '\nA:'

        # KK: context injection + logit boost tokens
        kk_context = None
        kk_boost_ids = []
        if kk:
            kk_context = kk.get_context(prompt_text)
            if kk_context:
                print(f'\n[kk] "{kk_context[:100]}..."')
                # extract KEY TERMS only — not all tokens
                import re
                words = re.findall(r'[A-Z][a-z]{3,}|[A-Z]{2,}|[a-z]{6,}', kk_context)
                key_terms = list(set(words))[:20]
                kk_boost_ids = []
                for term in key_terms:
                    kk_boost_ids.extend(tok.encode(term))
                kk_boost_ids = list(set(kk_boost_ids))
                print(f'[kk] key terms: {key_terms[:10]}')
                print(f'[kk] {len(kk_boost_ids)} boost token IDs')

        # build full prompt: KK context Q/A + our question
        if kk_context:
            full = kk_context + '\n' + prompt_text
        else:
            full = prompt_text

        ids = [BOS] + tok.encode(full)
        print(f'[{len(ids)} tokens]\n')

        # combined: context injection (model sees knowledge) +
        # logit boost (knowledge tokens get bias toward generation)
        generate(model, tok, ids, max_tokens=args.max_tokens,
                 kk_boost_ids=kk_boost_ids, kk_boost_strength=3.0,
                 **sample_kwargs)
        print('\n')

    if args.prompt:
        run_prompt(args.prompt)
    else:
        # interactive
        print('\nLeo v4 — interactive mode. Type your question. Ctrl+C to exit.\n')
        while True:
            try:
                prompt = input('you> ').strip()
                if not prompt:
                    continue
                run_prompt(prompt)
            except (KeyboardInterrupt, EOFError):
                print('\nbye.')
                break


if __name__ == '__main__':
    main()
