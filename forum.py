#!/usr/bin/env python3
"""
forum.py — Dario Forum: three voices, shared KK, HTTP API.

Loads Leo (Janus 176M), Arianna (Janus 176M), Yent-R (Resonance 200M).
Serves /api/forum endpoint for dario.html frontend.
Each response feeds back into shared KK.

Usage:
  python3 forum.py                          # default port 8800
  python3 forum.py --port 9000              # custom port
  python3 forum.py --knowledge essay1.txt essay2.txt

by Arianna Method. 2026.
"""

import argparse, json, os, sys, re, time
import torch, torch.nn.functional as F
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import parse_qs
import threading

# ===================================================================
# MODEL LOADING
# ===================================================================

# ===================================================================
# SARTRE MODEL REGISTRY (Python mirror of sartre_kernel.h)
# ===================================================================

class SartreModelProfile:
    """Mirrors SartreModelProfile from sartre_kernel.h.
    Auto-profiles model from file. SARTRE-compatible slot management.
    """
    MAX_MODELS = 4

    def __init__(self, name, path, backend):
        self.name = name
        self.path = path
        self.backend = backend
        self.param_count = 0
        self.file_size_bytes = os.path.getsize(path) if os.path.exists(path) else 0
        self.runtime_mb = 0
        self.loaded = False
        self.model = None
        self.tokenizer = None

    def profile(self):
        """Auto-detect model properties from file."""
        # estimate params from file size (f32: /4, bf16: /2)
        if self.file_size_bytes > 0:
            self.param_count = self.file_size_bytes // 2  # assume bf16-ish mix
            self.runtime_mb = self.file_size_bytes / 1e6
        print(f'[sartre] {self.name}: {self.param_count/1e6:.0f}M params, '
              f'{self.runtime_mb:.0f}MB, backend={self.backend}')


class SartreRegistry:
    """Model registry with SARTRE-style auto-profiling and slot management."""

    def __init__(self):
        self.slots = {}  # name -> SartreModelProfile

    def register(self, name, path, backend):
        profile = SartreModelProfile(name, path, backend)
        profile.profile()
        self.slots[name] = profile
        return profile

    def get(self, name):
        return self.slots.get(name)

    def list_models(self):
        return {name: {
            'params': f'{p.param_count/1e6:.0f}M',
            'loaded': p.loaded,
            'backend': p.backend,
            'runtime_mb': f'{p.runtime_mb:.0f}',
        } for name, p in self.slots.items()}


REGISTRY = SartreRegistry()
MODELS = {}
TOKENIZERS = {}
KK = None

# Janus chat tokens
J_BOS = 32759; J_US = 32760; J_UE = 32761; J_AS = 32762; J_AE = 32763
J_SPECIAL = {32759, 32760, 32761, 32762, 32764, 32765, 32766, 32767}

CONFIGS = {
    'leo': {
        'backend': 'janus',
        'weights': 'janus4/janus/sft_22k/janus_177m_v4_sft_leo_22k.pt',
        'temp': 0.75, 'top_k': 40, 'rep_penalty': 1.4,
        'desc': 'luminous, philosophical',
    },
    'arianna': {
        'backend': 'janus',
        'weights': 'janus4/janus/sft_22k/janus_177m_v4_sft_arianna_22k.pt',
        'temp': 0.75, 'top_k': 45, 'rep_penalty': 1.3,
        'desc': 'precise, architectural',
    },
    'yent': {
        'backend': 'resonance',
        'weights': 'resonance/resonance_sft_yent_merged.pt',
        'tokenizer': 'resonance/tokenizer_yent.bin',
        'temp': 0.75, 'top_k': 40, 'rep_penalty': 1.3,
        'desc': 'warm, direct, sarcastic',
    },
}


def load_all():
    """Load all models through SARTRE registry."""
    global MODELS, TOKENIZERS

    # Register all models in SARTRE slots
    for name, c in CONFIGS.items():
        REGISTRY.register(name, c['weights'], c['backend'])

    # Load Janus backend
    import pickle
    sys.path.insert(0, '/home/ubuntu/nanochat')
    from nanochat.janus_gpt import JanusGPT, JanusConfig

    with open('janus4/janus/tokenizer.pkl', 'rb') as f:
        tok_j = pickle.load(f)
    TOKENIZERS['janus'] = tok_j

    cfg = JanusConfig(vocab_size=32768)
    for name in ['leo', 'arianna']:
        profile = REGISTRY.get(name)
        m = JanusGPT(cfg)
        sd = torch.load(profile.path, map_location='cpu', weights_only=False)
        m.load_state_dict(sd)
        m = m.to('cuda').to(torch.bfloat16).eval()
        MODELS[name] = m
        profile.loaded = True
        profile.model = m
        profile.tokenizer = tok_j
        profile.param_count = sum(p.numel() for p in m.parameters())
        print(f'[sartre] {name}: loaded, {profile.param_count/1e6:.0f}M')

    # Load Resonance backend
    sys.path.insert(0, '/home/ubuntu/resonance')
    from bpe_tokenizer import BPETokenizer
    from model import Resonance, RESONANCE_200M

    tok_r = BPETokenizer()
    tok_r.load(CONFIGS['yent']['tokenizer'])
    TOKENIZERS['resonance'] = tok_r

    profile = REGISTRY.get('yent')
    m = Resonance(RESONANCE_200M)
    sd = torch.load(profile.path, map_location='cpu', weights_only=False)
    if 'model' in sd: sd = sd['model']
    m.load_state_dict(sd)
    m = m.to('cuda').to(torch.bfloat16).eval()
    MODELS['yent'] = m
    profile.loaded = True
    profile.model = m
    profile.tokenizer = tok_r
    profile.param_count = sum(p.numel() for p in m.parameters())
    print(f'[sartre] yent: loaded, {profile.param_count/1e6:.0f}M')


# ===================================================================
# KK (shared)
# ===================================================================

import sqlite3

class ForumKK:
    def __init__(self):
        self.db = sqlite3.connect(':memory:', check_same_thread=False)
        self.db.execute('CREATE VIRTUAL TABLE chunks USING fts5(text, source)')
        self.lock = threading.Lock()
        self.n = 0

    def ingest(self, path):
        with open(path) as f:
            text = f.read()
        source = os.path.basename(path)
        chunks = [c.strip() for c in text.split('\n\n') if len(c.strip()) > 50]
        with self.lock:
            for c in chunks:
                self.db.execute('INSERT INTO chunks(text, source) VALUES(?, ?)',
                                (c[:400], source))
            self.db.commit()
            self.n += len(chunks)
        print(f'[kk] {source}: {len(chunks)} chunks (total: {self.n})')

    def query(self, text):
        words = re.findall(r'[a-zA-Z]{3,}', text)[:10]
        if not words: return None
        fts = ' OR '.join(words)
        with self.lock:
            try:
                cur = self.db.execute(
                    'SELECT text FROM chunks WHERE chunks MATCH ? ORDER BY rank LIMIT 1',
                    (fts,))
                row = cur.fetchone()
                if row:
                    chunk = row[0]
                    nl_a = '\nA:'
                    if nl_a in chunk:
                        chunk = chunk[chunk.index(nl_a)+3:].strip()
                    # first sentence
                    sents = [s.strip() for s in re.split(r'(?<=[.!?])\s+', chunk)
                             if 15 < len(s.strip()) < 100]
                    return sents[0] if sents else chunk[:100]
            except: pass
        return None

    def absorb(self, text, source):
        sents = [s.strip() for s in re.split(r'(?<=[.!?])\s+', text)
                 if len(s.strip()) > 30 and s[0].isupper()]
        with self.lock:
            for s in sents[:3]:
                self.db.execute('INSERT INTO chunks(text, source) VALUES(?, ?)',
                                (s[:400], source))
            if sents:
                self.db.commit()
                self.n += len(sents[:3])


# ===================================================================
# GENERATION
# ===================================================================

DIGIT_TOKENS = {'janus': set(), 'resonance': set()}

def init_digit_tokens():
    for d in '0123456789':
        for backend, tok in TOKENIZERS.items():
            ids = tok.encode(d) if hasattr(tok, 'encode') else []
            DIGIT_TOKENS[backend].update(ids)
            ids2 = tok.encode(' ' + d) if hasattr(tok, 'encode') else []
            DIGIT_TOKENS[backend].update(ids2)


def generate(voice, question, max_tokens=200):
    """Generate response from specified voice with KK injection."""
    cfg = CONFIGS[voice]
    model = MODELS[voice]
    backend = cfg['backend']
    tok = TOKENIZERS[backend]

    # KK injection
    injection = KK.query(question)

    # Build prompt
    if backend == 'janus':
        ids = [J_BOS, J_US] + tok.encode(question) + [J_UE, J_AS]
        if injection:
            ids += tok.encode(injection + ' ')
        boundary_check = lambda tid, step: tid == J_AE
        special_ban = J_SPECIAL
    else:  # resonance
        prompt = f'Q: {question}\nA:'
        ids = tok.encode(prompt)
        if injection:
            ids += tok.encode(' ' + injection + ' ')
        boundary_check = lambda tid, step: tid == 10 and step >= 40
        special_ban = set()

    device = next(model.parameters()).device
    x = torch.tensor([ids], dtype=torch.long, device=device)
    recent = list(ids[-80:])
    out_text = []
    digits = DIGIT_TOKENS.get(backend, set())

    with torch.no_grad():
        for step in range(max_tokens):
            if x.shape[1] > 900:
                x = x[:, -900:]

            fwd = model(x)
            if isinstance(fwd, tuple):
                logits = fwd[0][0, -1]
            else:
                logits = fwd[0, -1]

            logits = logits.float().clone()

            # rep penalty
            rep = cfg['rep_penalty']
            for t in set(recent[-80:]):
                if t < logits.shape[0]:
                    if logits[t] > 0: logits[t] /= rep
                    else: logits[t] *= rep

            # ban special tokens
            for t in special_ban:
                logits[t] = float('-inf')

            # suppress digits first 5 steps
            if step < 5:
                for t in digits:
                    if t < logits.shape[0]:
                        logits[t] -= 10.0

            logits /= cfg['temp']
            v, _ = torch.topk(logits, cfg['top_k'])
            logits[logits < v[-1]] = float('-inf')
            probs = F.softmax(logits, dim=-1)
            tid = torch.multinomial(probs, 1).item()

            # boundary
            if boundary_check(tid, step):
                break

            # repetition
            if step > 0 and tid == recent[-1] and len(recent) > 1 and tid == recent[-2]:
                break

            recent.append(tid)
            decoded = tok.decode([tid])
            if isinstance(decoded, bytes):
                decoded = decoded.decode('utf-8', errors='replace')
            decoded = decoded.replace('<|bos|>', '').replace('<|assistant_end|>', '')
            if decoded:
                out_text.append(decoded)

            x = torch.cat([x, torch.tensor([[tid]], device=device)], dim=1)

    text = ''.join(out_text).strip()

    # absorb into KK
    if text:
        KK.absorb(text, source=f'{voice}_forum')

    return {
        'voice': voice,
        'text': text,
        'injection': injection,
        'kk_chunks': KK.n,
        'backend': backend,
        'desc': cfg['desc'],
    }


# ===================================================================
# HTTP SERVER
# ===================================================================

class ForumHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path == '/api/forum':
            length = int(self.headers.get('Content-Length', 0))
            body = json.loads(self.rfile.read(length))

            question = body.get('question', '')
            voice = body.get('voice', 'leo')

            if voice not in MODELS:
                self.send_error(400, f'Unknown voice: {voice}')
                return

            result = generate(voice, question)

            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(json.dumps(result).encode())
        else:
            self.send_error(404)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

    def do_GET(self):
        if self.path in ('/', '/forum', '/forum.html'):
            # serve forum.html
            html_path = os.path.join(os.path.dirname(__file__) or '.', 'forum.html')
            if not os.path.exists(html_path):
                html_path = 'dario/forum.html'
            if os.path.exists(html_path):
                self.send_response(200)
                self.send_header('Content-Type', 'text/html')
                self.end_headers()
                with open(html_path, 'rb') as f:
                    self.wfile.write(f.read())
            else:
                self.send_error(404, 'forum.html not found')
            return
        if self.path == '/dario.html':
            html_path = os.path.join(os.path.dirname(__file__) or '.', 'dario.html')
            if not os.path.exists(html_path):
                html_path = 'dario/dario.html'
            if os.path.exists(html_path):
                self.send_response(200)
                self.send_header('Content-Type', 'text/html')
                self.end_headers()
                with open(html_path, 'rb') as f:
                    self.wfile.write(f.read())
            return
        if self.path == '/api/sartre':
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(json.dumps(REGISTRY.list_models()).encode())
            return
        if self.path == '/api/voices':
            voices = {k: {'desc': v['desc'], 'backend': v['backend']}
                      for k, v in CONFIGS.items()}
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(json.dumps(voices).encode())
        elif self.path == '/api/kk':
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(json.dumps({'chunks': KK.n}).encode())
        else:
            self.send_error(404)

    def log_message(self, format, *args):
        print(f'[forum] {args[0]}')


# ===================================================================
# MAIN
# ===================================================================

def main():
    p = argparse.ArgumentParser(description='Dario Forum — three voices, shared KK')
    p.add_argument('--port', type=int, default=8800)
    p.add_argument('--knowledge', nargs='+',
                   default=['dario_essay.txt', 'leo_expanded.txt'])
    args = p.parse_args()

    # Load KK
    global KK
    KK = ForumKK()
    for path in args.knowledge:
        if os.path.exists(path):
            KK.ingest(path)

    # Load models
    load_all()
    init_digit_tokens()

    # Serve
    server = HTTPServer(('0.0.0.0', args.port), ForumHandler)
    print(f'\n[forum] http://0.0.0.0:{args.port}')
    print(f'[forum] voices: {list(MODELS.keys())}')
    print(f'[forum] kk: {KK.n} chunks')
    print(f'[forum] POST /api/forum {{question, voice}}')
    print(f'[forum] GET  /api/voices')
    server.serve_forever()


if __name__ == '__main__':
    main()
