"""
test_v4_kk.py — Leo v4 176M + Knowledge Kernel on H100

End-to-end: model generates, KK provides knowledge via context injection.
Compare generation WITH and WITHOUT KK.

PYTHONPATH=/home/ubuntu/nanochat python3 test_v4_kk.py
"""
import pickle, torch, sqlite3, os, sys
sys.path.insert(0, '/home/ubuntu/nanochat')
from nanochat.janus_gpt import JanusGPT, JanusConfig

# === LOAD MODEL ===
with open('janus4/janus/tokenizer.pkl', 'rb') as f:
    tok = pickle.load(f)

cfg = JanusConfig(vocab_size=32768)
model = JanusGPT(cfg)
sd = torch.load('janus4/janus/sft_22k/janus_177m_v4_sft_leo_22k.pt',
                map_location='cpu', weights_only=False)
model.load_state_dict(sd)
model = model.to('cuda').to(torch.bfloat16).eval()
BOS = 32766  # <|output_start|>
EOS = 32767  # <|output_end|>
print('[model] Leo v4 176M loaded — RRPRAM + Echo + 3-way gate')

# === SETUP KK (Python FTS5 — same concept as C kk_kernel) ===
db = sqlite3.connect(':memory:')
db.execute('CREATE VIRTUAL TABLE chunks USING fts5(text)')

# Ingest leo_expanded.txt
leo_path = None
for p in ['leo_expanded.txt', '/tmp/leo_expanded.txt',
          os.path.expanduser('~/Downloads/leo_expanded.txt')]:
    if os.path.exists(p):
        leo_path = p
        break

if leo_path:
    with open(leo_path, 'r') as f:
        text = f.read()
    print(f'[kk] loaded {leo_path}: {len(text)} chars')
else:
    print('[kk] ERROR: leo_expanded.txt not found')
    sys.exit(1)

# Chunk by double newlines (same as kk_kernel.c)
chunks = [c.strip() for c in text.split('\n\n') if len(c.strip()) > 50]
for c in chunks:
    db.execute('INSERT INTO chunks(text) VALUES(?)', (c[:500],))
db.commit()
print(f'[kk] {len(chunks)} chunks indexed\n')


def kk_query(query_text, top_k=3):
    """FTS5 search — same scoring concept as kk_kernel.c"""
    # clean query for FTS5
    words = ''.join(c if c.isalnum() or c == ' ' else ' ' for c in query_text).split()
    if not words:
        return []
    fts_query = ' OR '.join(words[:8])
    try:
        cur = db.execute(
            'SELECT text, rank FROM chunks WHERE chunks MATCH ? ORDER BY rank LIMIT ?',
            (fts_query, top_k))
        return [(row[0], row[1]) for row in cur.fetchall()]
    except:
        return []


def generate(prompt, max_tokens=200, temp=0.8):
    """Generate without KK"""
    ids = [BOS] + tok.encode(prompt)
    out = []
    with torch.no_grad():
        for i, tid in enumerate(model.generate(ids, max_tokens=max_tokens,
                                                temperature=temp, top_k=50)):
            if tid == EOS or i >= max_tokens:
                break
            out.append(tok.decode([tid]))
            print(tok.decode([tid]), end='', flush=True)
    return ''.join(out)


def generate_with_kk(prompt, max_tokens=200, temp=0.8):
    """Generate with KK context injection"""
    # Query KK
    results = kk_query(prompt, top_k=2)

    context = ''
    if results:
        chunk = results[0][0]
        # Take answer part if Q/A format
        if 'A:' in chunk:
            answer = chunk[chunk.index('A:'):]
        else:
            answer = chunk
        if len(answer) > 500:
            answer = answer[:500]
        context = answer + '\n'
        print(f'[kk] injected: "{answer[:100]}..."')

    # Build full prompt with knowledge context
    full = context + prompt
    ids = [BOS] + tok.encode(full)
    print(f'[ctx] {len(ids)} tokens\n')

    out = []
    with torch.no_grad():
        for i, tid in enumerate(model.generate(ids, max_tokens=max_tokens,
                                                temperature=temp, top_k=50)):
            if tid == EOS or i >= max_tokens:
                break
            out.append(tok.decode([tid]))
            print(tok.decode([tid]), end='', flush=True)
    return ''.join(out)


# === TESTS ===
print('=' * 60)
print('  TEST 1: WITHOUT KK — "What is RRPRAM?"')
print('=' * 60)
generate('Q: What is RRPRAM?\nA:')
print('\n')

print('=' * 60)
print('  TEST 2: WITH KK — "What is RRPRAM?"')
print('=' * 60)
generate_with_kk('Q: What is RRPRAM?\nA:')
print('\n')

print('=' * 60)
print('  TEST 3: WITH KK — "How does echo attention work?"')
print('=' * 60)
generate_with_kk('Q: How does echo attention work?\nA:')
print('\n')

print('=' * 60)
print('  TEST 4: WITH KK — "What is the gate mechanism?"')
print('=' * 60)
generate_with_kk('Q: What is the gate mechanism in Janus?\nA:')
print('\n\ndone.')
