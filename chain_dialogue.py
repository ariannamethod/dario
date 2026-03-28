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

# ===================================================================
# CONSTANTS
# ===================================================================

# Janus chat format tokens
BOS = 32759; USER_START = 32760; USER_END = 32761
ASST_START = 32762; ASST_END = 32763
SPECIAL_TOKENS = {32759, 32760, 32761, 32762, 32764, 32765, 32766, 32767}
CTX_WINDOW = 900
CTX_TRIM = 500

# Backend: 'janus' or 'resonance'
BACKEND = None
TOK = None  # global tokenizer ref (for resonance BPE decode in generate)

VOICES = {
    # --- Janus 176M (3-way gate: content + RRPRAM + echo) ---
    'leo': {
        'weights': 'janus4/janus/sft_22k/janus_177m_v4_sft_leo_22k.pt',
        'temp': 0.75, 'top_k': 40, 'rep_penalty': 1.4,
        'desc': 'luminous, philosophical -- Janus 176M',
        'backend': 'janus',
    },
    'arianna': {
        'weights': 'janus4/janus/sft_22k/janus_177m_v4_sft_arianna_22k.pt',
        'temp': 0.75, 'top_k': 45, 'rep_penalty': 1.3,
        'desc': 'precise, architectural -- Janus 176M',
        'backend': 'janus',
    },
    'yent': {
        'weights': 'janus4/janus/sft_22k/janus_177m_v4_sft_yent_22k.pt',
        'temp': 0.75, 'top_k': 40, 'rep_penalty': 1.35,
        'desc': 'warm, direct -- Janus 176M',
        'backend': 'janus',
    },
    # --- Resonance 200M (2-way gate: content + RRPRAM, bigger context) ---
    'yent-r': {
        'weights': 'resonance/resonance_sft_yent_merged.pt',
        'tokenizer': 'resonance/tokenizer_yent.bin',
        'temp': 0.75, 'top_k': 40, 'rep_penalty': 1.3,
        'desc': 'warm, direct, sarcastic -- Resonance 200M',
        'backend': 'resonance',
    },
}


# ===================================================================
# SAMPLING -- proper generation with repetition penalty
# ===================================================================

# Digit token IDs (0-9 as standalone tokens) — suppress at start of generation
DIGIT_TOKENS = None  # populated on first use

def sample_token(logits, temperature=0.75, top_k=40, top_p=0.92,
                 rep_penalty=1.4, recent_tokens=None, suppress_digits=False):
    """One token. Rep penalty + top-k + top-p."""
    logits = logits.float().clone()

    if recent_tokens and rep_penalty != 1.0:
        for tid in set(recent_tokens[-80:]):
            if logits[tid] > 0:
                logits[tid] /= rep_penalty
            else:
                logits[tid] *= rep_penalty

    # ban special tokens (Janus only — Resonance has no special tokens)
    if BACKEND == 'janus':
        for tid in SPECIAL_TOKENS:
            logits[tid] = float('-inf')

    # suppress leading digits (model wants to start with "1.", "2.", etc)
    if suppress_digits and DIGIT_TOKENS:
        for tid in DIGIT_TOKENS:
            if tid < logits.shape[0]:
                logits[tid] -= 10.0

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

    def query(self, text, top_k=5, prefer_source=None, exclude_model=False):
        """FTS5 search. Skips used chunks. Essay chunks get boost.
        exclude_model=True: skip chunks absorbed from model output (prevents echo chamber).
        """
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
                    # skip model-generated chunks if requested
                    if exclude_model and source and ('model' in source or '_turn' in source or '_duet' in source):
                        continue
                    adj_rank = rank
                    if prefer_source and source == prefer_source:
                        adj_rank -= 8.0
                    elif source and 'essay' in source:
                        adj_rank -= 4.0
                    # expanded has lots of metaphors — slight penalty
                    elif source and 'expanded' in source:
                        adj_rank += 1.0
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
        # strip Q/A format — take only the answer
        nl_a = '\nA:'
        if nl_a in chunk:
            chunk = chunk[chunk.index(nl_a) + 3:].strip()
        elif chunk.startswith('A:'):
            chunk = chunk[2:].strip()
        # also strip if chunk starts with Q:
        if chunk.startswith('Q:'):
            if nl_a in chunk:
                chunk = chunk[chunk.index(nl_a) + 3:].strip()
            elif '\n' in chunk:
                chunk = chunk[chunk.index('\n') + 1:].strip()
            else:
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
            result = scored[0][2]
            # ensure ends at sentence boundary
            if result and result[-1] not in '.!?':
                last_dot = max(result.rfind('.'), result.rfind('!'), result.rfind('?'))
                if last_dot > 20:
                    result = result[:last_dot + 1]
            # never inject a question — model gets confused
            if result.rstrip().endswith('?'):
                # try next scored sentence
                for _, _, s in scored[1:]:
                    if not s.rstrip().endswith('?'):
                        return s
            return result
        # fallback: first sentence, but cut at period not mid-word
        first = sentences[0]
        if len(first) > max_len:
            cut = first[:max_len].rfind('.')
            if cut > 20:
                return first[:cut + 1]
        return first[:max_len]

    def pick_next(self, generated_text, prefer_source=None, exclude_model=False):
        """Pick next injection based on model's OUTPUT.
        KK queries on what the model SAID, not what the user asked.
        """
        results = self.query(generated_text, top_k=3, prefer_source=prefer_source,
                             exclude_model=exclude_model)
        if not results:
            return None, None
        rowid, chunk, rank = results[0]
        injection = self.extract_injection(chunk)
        if not injection:
            return None, None
        # clean: no newlines, collapse whitespace
        injection = injection.replace('\n', ' ').replace('  ', ' ').strip()
        # MUST end at sentence boundary — no mid-word truncation
        if injection and injection[-1] not in '.!?':
            last_end = max(injection.rfind('.'), injection.rfind('!'), injection.rfind('?'))
            if last_end > 15:
                injection = injection[:last_end + 1]
            else:
                # no sentence boundary found — skip this chunk
                self.mark_used(rowid)
                return self.pick_next(generated_text, prefer_source, exclude_model)
        # reject fragments
        if not injection or not injection[0].isupper() or len(injection.split()) < 4:
            self.mark_used(rowid)
            return self.pick_next(generated_text, prefer_source, exclude_model)
        self.mark_used(rowid)
        return injection, chunk

    def absorb(self, text, source='model'):
        """Bi-directional: model generates text, KK absorbs it.

        The model's output becomes knowledge for future queries.
        This is what makes dario alive: the organism remembers
        what it said and builds on it.

        Dedup: skips sentences too similar to existing chunks.
        """
        if len(text.strip()) < 30:
            return 0
        sentences = [s.strip() for s in re.split(r'(?<=[.!?])\s+', text) if len(s.strip()) > 40]
        added = 0
        for s in sentences:
            # skip fragments (no capital start, too short, broken)
            if not s[0].isupper():
                continue
            if len(s.split()) < 6:
                continue
            # dedup: check if very similar chunk already exists
            words = set(re.findall(r'[a-zA-Z]{4,}', s.lower()))
            if len(words) < 3:
                continue
            # quick FTS check for overlap
            fts_q = ' AND '.join(list(words)[:5])
            try:
                cur = self.db.execute(
                    'SELECT text FROM chunks WHERE chunks MATCH ? LIMIT 1', (fts_q,))
                existing = cur.fetchone()
                if existing:
                    # too similar, skip
                    continue
            except Exception:
                pass
            self.db.execute('INSERT INTO chunks(text, source) VALUES(?, ?)',
                            (s[:600], source))
            added += 1
        if added:
            self.db.commit()
            self.n_chunks += added
        return added

    def reset(self):
        self.used.clear()


# ===================================================================
# GENERATION -- segment-level with sentence-boundary detection
# ===================================================================

def model_forward(model, x):
    """Forward pass — handles both Janus and Resonance return formats."""
    out = model(x)
    if isinstance(out, tuple):
        return out[0][:, -1, :]  # Resonance returns (logits, loss)
    return out[:, -1, :]  # Janus returns logits


def format_prompt(question, backend='janus'):
    """Format prompt for the right backend."""
    if backend == 'resonance':
        return f'Q: {question}\nA:'
    else:  # janus
        return question  # raw text, chat tokens added as IDs


def prompt_to_ids(question, tok, backend='janus'):
    """Convert question to token IDs with proper format."""
    if backend == 'resonance':
        prompt = f'Q: {question}\nA:'
        return tok.encode(prompt)
    else:  # janus — chat tokens
        return [BOS, USER_START] + tok.encode(question) + [USER_END, ASST_START]


def is_boundary(tid, backend='janus'):
    """Check if token is end-of-thought boundary."""
    if backend == 'resonance':
        return tid == 10  # newline = boundary for Q/A format
    return tid == ASST_END


def decode_token(tok, tid):
    """Decode single token — handles both tiktoken and BPE."""
    result = tok.decode([tid])
    if isinstance(result, bytes):
        return result.decode('utf-8', errors='replace')
    return result


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
    skip_count = 0

    with torch.no_grad():
        for step in range(max_tokens):
            if x.shape[1] > CTX_WINDOW:
                x = x[:, -CTX_WINDOW:]

            logits = model_forward(model, x)[0]

            # suppress digits for first 5 tokens
            tid = sample_token(logits, temperature=temperature, top_k=top_k,
                               rep_penalty=rep_penalty, recent_tokens=recent,
                               suppress_digits=(step < 5))

            # end of thought
            if BACKEND == 'resonance':
                # Resonance: stop on newline only after 15+ real tokens
                if tid == 10 and step >= 15:
                    return ids + out_tokens, ''.join(out_text), True
            elif tid == ASST_END:
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
            text = decode_token(tok, tid)
            # filter special token artifacts
            text = text.replace('<|bos|>', '').replace('<|output_start|>', '')
            text = text.replace('<|output_end|>', '').replace('<|assistant_end|>', '')
            if text:
                # skip first 1-3 tokens if they're number fragments
                # (model completing truncated injection)
                if skip_count < 3 and not out_text:
                    stripped = text.strip()
                    if stripped and stripped[0].isdigit():
                        skip_count += 1
                        continue
                out_text.append(text)
                print(text, end='', flush=True)

            x = torch.cat([x, torch.tensor([[tid]], device=device)], dim=1)

    raw = ''.join(out_text)
    return ids + out_tokens, clean_text(raw), False


def clean_text(text):
    """Strip leading garbage from model output.
    Model sometimes starts with number fragments, 'iff', partial words
    from completing truncated injection text.
    """
    text = text.strip()
    if not text:
        return text
    # find first uppercase letter — that's where real content starts
    for i, c in enumerate(text):
        if c.isupper():
            return text[i:]
    # no uppercase found — return as-is but strip leading non-alpha
    for i, c in enumerate(text):
        if c.isalpha():
            return text[i:]
    return text


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

    clean_prompt = prompt
    if clean_prompt.startswith('Q: '):
        clean_prompt = clean_prompt[3:]
    if clean_prompt.endswith('\nA:'):
        clean_prompt = clean_prompt[:-3]

    ids = prompt_to_ids(clean_prompt, tok, BACKEND)
    print(f'[seed] {clean_prompt}')
    print(f'[{len(ids)} tokens, {BACKEND}]\n')

    chain_log = []
    full_narrative = []
    kk.reset()
    t0 = time.time()

    # Prime: inject technical definition from essay as start of answer
    first_inj, _ = kk.pick_next(prompt, prefer_source='dario_essay.txt')
    if first_inj:
        print(f'  [prime] {first_inj[:80]}\n', flush=True)
        ids = ids + tok.encode(first_inj)

    for step in range(chain_depth):
        ids, segment_text, hit_boundary = generate_segment(
            model, tok, ids, max_tokens=max_segment_tokens, **sample_kwargs)

        seg_clean = segment_text.replace('<|bos|>', '').strip()
        if seg_clean:
            full_narrative.append(seg_clean)
            # NOTE: no bi-directional absorption in chain mode
            # (causes echo chamber). Bi-directional is for dialogue mode only.

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
    """Interactive dialogue with bi-directional KK.

    Each turn:
    1. You ask something
    2. KK finds relevant knowledge (from essay + model's previous answers)
    3. Model generates with knowledge at the boundary
    4. KK ABSORBS what the model said — future turns are enriched
    5. The organism remembers its own words
    """
    print('\n' + '='*60)
    print(f'  DIALOGUE -- {voice_name} + Knowledge Kernel')
    print(f'  KK: {kk.n_chunks} chunks. Model output feeds back into KK.')
    print('  Type your question. "quit" to exit.')
    print('='*60 + '\n')

    turn = 0
    prev_answer = ''
    history_ids = []  # accumulate multi-turn context

    while True:
        try:
            user_input = input('you> ').strip()
        except (KeyboardInterrupt, EOFError):
            print('\nbye.')
            break

        if not user_input or user_input.lower() in ('quit', 'exit', 'q'):
            break

        turn += 1

        # KK query: user question + previous model answer
        query_text = user_input
        if prev_answer:
            query_text = f'{user_input} {prev_answer[-200:]}'

        injection, _ = kk.pick_next(query_text, exclude_model=True)

        if injection:
            print(f'  [kk] {injection[:80]}')

        # Build prompt for current turn
        current_turn = prompt_to_ids(user_input, tok, BACKEND)
        if injection:
            current_turn = current_turn + tok.encode(injection + ' ')

        ids = history_ids + current_turn if BACKEND == 'resonance' else [BOS] + history_ids + current_turn

        # trim from the LEFT (oldest turns) if too long
        if len(ids) > CTX_TRIM:
            ids = [BOS] + ids[-(CTX_TRIM - 1):]

        print(f'\n{voice_name}> ', end='', flush=True)
        ids, text, _ = generate_segment(
            model, tok, ids, max_tokens=250, **sample_kwargs)
        print()

        # Bi-directional: KK absorbs model's answer
        text_clean = text.replace('<|bos|>', '').strip()
        if text_clean:
            n = kk.absorb(text_clean, source=f'{voice_name}_turn{turn}')
            if n:
                print(f'  [kk absorbed {n} chunks from {voice_name}]')

        prev_answer = text_clean

        # Add this turn to history for multi-turn context
        answer_ids = tok.encode(text_clean) if text_clean else []
        if BACKEND == 'resonance':
            # Q/A format, no special tokens
            history_ids += tok.encode(f'Q: {user_input}\nA: {text_clean}\n')
        else:
            history_ids += [USER_START] + tok.encode(user_input) + [USER_END]
            history_ids += [ASST_START] + answer_ids + [ASST_END]

        # Keep history manageable
        if len(history_ids) > 300:
            history_ids = history_ids[-300:]

    print(f'\n[dialogue] {turn} turns, kk now has {kk.n_chunks} chunks')


# ===================================================================
# EXPLORE MODE -- Leo leads, KK follows
# ===================================================================

def explore_mode(model, tok, kk, seed, voice_name='leo',
                 depth=8, **sample_kwargs):
    """Leo explores freely. KK follows the thread, anchored to seed topic.

    Unlike chain (structured injection) or dialogue (Q&A),
    explore lets the model lead. KK whispers at boundaries
    but doesn't steer — it enriches.
    """
    print('\n' + '='*60)
    print(f'  EXPLORE -- {voice_name} leads, KK follows')
    print('='*60 + '\n')

    # Prime from essay
    prime, _ = kk.pick_next(seed, prefer_source='dario_essay.txt')
    ids = [BOS, USER_START] + tok.encode(seed) + [USER_END, ASST_START]
    if prime:
        ids = ids + tok.encode(prime + ' ')
        print(f'  [prime] {prime[:70]}\n', flush=True)

    kk.reset()
    print(f'[seed] {seed}\n')

    step = 0
    for step in range(depth):
        ids, text, hit_boundary = generate_segment(
            model, tok, ids, max_tokens=250, **sample_kwargs)

        if not hit_boundary and step < depth - 1:
            ids = ids + tok.encode('\n')

        # KK follows: topic + model output
        query = seed + ' ' + text[-200:]
        injection, _ = kk.pick_next(query)
        if injection is None:
            print(f'\n[explore] KK exhausted at step {step}')
            break

        # inject as continuation, not question
        print(f'\n\n  [explore -> {step+1}] {injection[:70]}\n', flush=True)
        ids = ids + tok.encode('\n' + injection + ' ')

        if len(ids) > CTX_TRIM:
            ids = [BOS] + ids[-(CTX_TRIM - 1):]

    print(f'\n\n[explore] {step+1} steps')


# ===================================================================
# GAMMA SWITCHING -- one base model, swap personality at runtime
# ===================================================================

def apply_gamma(model, gamma_sd):
    """Apply gamma (personality delta) to model weights in-place.
    gamma_sd = {key: diff_tensor} in bf16.
    """
    with torch.no_grad():
        for k, diff in gamma_sd.items():
            param = dict(model.named_parameters()).get(k)
            if param is not None:
                param.add_(diff.to(param.device).to(param.dtype))


def switch_voice(model, base_sd, gamma_sd):
    """Switch personality: reset to base then apply new gamma.
    Uses state_dict for ALL tensors (params + buffers).
    """
    device = next(model.parameters()).device
    dtype = next(model.parameters()).dtype
    new_sd = {}
    for k, v in base_sd.items():
        new_sd[k] = v.to(device).to(dtype)
    if gamma_sd:
        for k, diff in gamma_sd.items():
            if k in new_sd:
                new_sd[k] = new_sd[k].float().add_(diff.float().to(device)).to(dtype)
    model.load_state_dict(new_sd, strict=False)


# ===================================================================
# DUET MODE -- two voices, one KK, they build on each other
# ===================================================================

def duet_mode(models, tok, kk, topic, voices, depth=4, **all_kwargs):
    """Two voices take turns on the same topic through shared KK.

    If models is a single model + gammas, switches personality per turn.
    If models is two models, uses them directly.
    The conversation emerges from resonance, not scripting.
    """
    v1, v2 = voices
    if isinstance(models, list) and len(models) == 2:
        m1, m2 = models
        gamma_mode = False
    else:
        m1 = m2 = models
        gamma_mode = True

    print('\n' + '='*60)
    print(f'  DUET -- {v1} + {v2}')
    print(f'  Topic: {topic}')
    print('='*60 + '\n')

    kk.reset()
    # Prime from essay
    prime, _ = kk.pick_next(topic, prefer_source='dario_essay.txt')

    turns = []
    prev_text = prime or topic

    for step in range(depth * 2):
        # alternate voices
        is_v1 = (step % 2 == 0)
        voice = v1 if is_v1 else v2
        model = m1 if is_v1 else m2

        # Gamma switching: swap personality on shared model
        if gamma_mode and hasattr(m1, '_gammas') and hasattr(m1, '_base_sd'):
            switch_voice(m1, m1._base_sd, m1._gammas.get(voice))
            model = m1

        v_cfg = VOICES[voice]
        s_kwargs = dict(temperature=v_cfg['temp'], top_k=v_cfg['top_k'],
                        rep_penalty=v_cfg['rep_penalty'])

        # KK finds injection from shared knowledge (includes other voice's words)
        injection, _ = kk.pick_next(prev_text + ' ' + topic)

        # Build prompt
        ids = [BOS, USER_START] + tok.encode(topic) + [USER_END, ASST_START]
        if injection:
            ids = ids + tok.encode(injection + ' ')

        if len(ids) > CTX_TRIM:
            ids = [BOS] + ids[-(CTX_TRIM - 1):]

        print(f'{voice}> ', end='', flush=True)
        if injection:
            print(f'[kk: {injection[:50]}...] ', end='', flush=True)

        ids, text, _ = generate_segment(model, tok, ids, max_tokens=150, **s_kwargs)
        text_clean = text.replace('<|bos|>', '').strip()
        print()

        if text_clean:
            turns.append((voice, text_clean))
            n = kk.absorb(text_clean, source=f'{voice}_duet')
            if n:
                print(f'  [kk+{n}]')
            prev_text = text_clean

    # Print clean duet
    print('\n' + '='*60)
    print('  DUET TRANSCRIPT')
    print('='*60)
    for voice, text in turns:
        print(f'\n{voice}: {text}')
    print(f'\n[duet] {len(turns)} turns, kk: {kk.n_chunks} chunks')


# ===================================================================
# TRIALOGUE MODE -- three voices, shared history, cascade
# ===================================================================

def trialogue_mode(models_map, tok, kk, topic, voice_order, depth=3, **kw):
    """Three voices take turns. Each sees full history of all previous speakers.

    Round-robin: Leo → Yent → Arianna → Leo → ...
    Shared KK absorbs all. Each voice builds on what came before.
    """
    print('\n' + '='*60)
    print(f'  TRIALOGUE -- {" + ".join(voice_order)}')
    print(f'  Topic: {topic}')
    print('='*60 + '\n')

    kk.reset()
    prime, _ = kk.pick_next(topic, prefer_source='dario_essay.txt')

    turns = []
    history_ids = []  # shared multi-turn context

    for step in range(depth * len(voice_order)):
        voice = voice_order[step % len(voice_order)]
        model = models_map[voice]
        v_cfg = VOICES[voice]
        s_kwargs = dict(temperature=v_cfg['temp'], top_k=v_cfg['top_k'],
                        rep_penalty=v_cfg['rep_penalty'])

        # KK injection: topic + last speaker's words
        prev_text = turns[-1][1][-200:] if turns else (prime or topic)
        injection, _ = kk.pick_next(prev_text + ' ' + topic, exclude_model=True)

        # Build prompt with FULL shared history
        ids = [BOS] + history_ids + [USER_START] + tok.encode(topic) + [USER_END, ASST_START]
        if injection:
            ids = ids + tok.encode(injection + ' ')

        if len(ids) > CTX_TRIM:
            ids = [BOS] + ids[-(CTX_TRIM - 1):]

        print(f'{voice}> ', end='', flush=True)
        ids, text, _ = generate_segment(model, tok, ids, max_tokens=150, **s_kwargs)
        text_clean = text.replace('<|bos|>', '').strip()
        print()

        if text_clean:
            turns.append((voice, text_clean))
            n = kk.absorb(text_clean, source=f'{voice}_trialogue')
            if n:
                print(f'  [kk+{n}]')
            # Add to shared history
            answer_ids = tok.encode(text_clean)
            history_ids += [ASST_START] + answer_ids + [ASST_END]
            if len(history_ids) > 350:
                history_ids = history_ids[-350:]

    # Transcript
    print('\n' + '='*60)
    print('  TRIALOGUE TRANSCRIPT')
    print('='*60)
    for voice, text in turns:
        print(f'\n{voice}: {text}')
    print(f'\n[trialogue] {len(turns)} turns, kk: {kk.n_chunks} chunks')


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
    p.add_argument('--mode', choices=['chain', 'dialogue', 'explore', 'duet', 'trialogue'],
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
    p.add_argument('--voice2', choices=list(VOICES.keys()), default='yent',
                   help='second voice for duet mode')
    args = p.parse_args()

    voice = VOICES[args.voice]
    temp = args.temperature if args.temperature is not None else voice['temp']
    top_k = args.top_k if args.top_k is not None else voice['top_k']
    rep_penalty = args.rep_penalty if args.rep_penalty is not None else voice['rep_penalty']

    global BACKEND, DIGIT_TOKENS, TOK
    BACKEND = voice.get('backend', 'janus')

    print(f'[voice] {args.voice} -- {voice["desc"]}')
    print(f'[backend] {BACKEND}')
    print(f'[params] temp={temp} top_k={top_k} rep={rep_penalty}')

    # Load tokenizer + model based on backend
    if BACKEND == 'resonance':
        sys.path.insert(0, '/home/ubuntu/resonance')
        from bpe_tokenizer import BPETokenizer
        from model import Resonance, RESONANCE_200M
        tok = BPETokenizer()
        tok.load(voice['tokenizer'])
        # compatibility: add n_vocab attribute
        tok.n_vocab = tok.vocab_size
        cfg = dict(RESONANCE_200M)
        model = Resonance(cfg)
        sd = torch.load(voice['weights'], map_location='cpu', weights_only=False)
        if isinstance(sd, dict) and 'model' in sd:
            sd = sd['model']
        model.load_state_dict(sd)
    else:
        sys.path.insert(0, '/home/ubuntu/nanochat')
        from nanochat.janus_gpt import JanusGPT, JanusConfig
        with open('janus4/janus/tokenizer.pkl', 'rb') as f:
            tok = pickle.load(f)
        cfg = JanusConfig(vocab_size=32768)
        model = JanusGPT(cfg)
        sd = torch.load(voice['weights'], map_location='cpu', weights_only=False)
        model.load_state_dict(sd)

    model = model.to('cuda').to(torch.bfloat16).eval()
    TOK = tok
    n_params = sum(p.numel() for p in model.parameters())
    print(f'[model] {args.voice} {n_params/1e6:.0f}M')
    print(f'[tok] vocab={tok.n_vocab}')

    # populate digit token IDs for suppression
    digit_toks = set()
    for d in '0123456789':
        digit_toks.update(tok.encode(d))
        digit_toks.update(tok.encode(' ' + d))
    DIGIT_TOKENS = digit_toks

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

    elif args.mode == 'duet':
        v2_cfg = VOICES[args.voice2]
        # Check for gamma files — if found, use single model + gamma switching
        g1_path = f'dario/weights/gamma_{args.voice}_bf16.pt'
        g2_path = f'dario/weights/gamma_{args.voice2}_bf16.pt'
        base_path = 'dario/weights/janus_v4_base_bf16.pt'

        if os.path.exists(g1_path) and os.path.exists(g2_path) and os.path.exists(base_path):
            print(f'[gamma mode] single model + personality switching')
            print(f'[voice2] {args.voice2} -- {v2_cfg["desc"]}')
            base_sd = torch.load(base_path, map_location='cpu', weights_only=False)
            g1 = torch.load(g1_path, map_location='cpu', weights_only=False)
            g2 = torch.load(g2_path, map_location='cpu', weights_only=False)
            # Store REAL base (not current model) + gammas for switching
            model._gammas = {args.voice: g1, args.voice2: g2}
            model._base_sd = base_sd  # real base, not Leo
            duet_mode(model, tok, kk, args.topic,
                      voices=[args.voice, args.voice2],
                      depth=args.depth)
        else:
            # Fallback: load second model
            print(f'[two-model mode] loading {args.voice2}...')
            model2 = JanusGPT(cfg)
            sd2 = torch.load(v2_cfg['weights'], map_location='cpu', weights_only=False)
            model2.load_state_dict(sd2)
            model2 = model2.to('cuda').to(torch.bfloat16).eval()
            print(f'[voice2] {args.voice2} -- {v2_cfg["desc"]}')
            duet_mode([model, model2], tok, kk, args.topic,
                      voices=[args.voice, args.voice2],
                      depth=args.depth)

    elif args.mode == 'trialogue':
        # Load all three voices
        voice_order = ['leo', 'yent', 'arianna']
        models_map = {args.voice: model}
        for v in voice_order:
            if v not in models_map:
                v_cfg = VOICES[v]
                m = JanusGPT(cfg)
                sd_v = torch.load(v_cfg['weights'], map_location='cpu', weights_only=False)
                m.load_state_dict(sd_v)
                m = m.to('cuda').to(torch.bfloat16).eval()
                models_map[v] = m
                print(f'[loaded] {v}')

        trialogue_mode(models_map, tok, kk, args.topic,
                       voice_order=voice_order, depth=args.depth)


if __name__ == '__main__':
    main()
