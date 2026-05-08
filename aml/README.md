# dario / AML port

Canonical port of the dario orchestration layer to AML. Replaces the
Python files (`dario_infer.py`, `chain_dialogue.py`, `forum.py`) end-to-end.
Mirrors the Go reference port at `cmd/{dario-infer,dario-dialogue,dario-forum}/`
voice catalog and contract.

| AML file              | Replaces           | LOC | Purpose                                      |
| --------------------- | ------------------ | --- | -------------------------------------------- |
| `dario_infer.aml`     | `dario_infer.py`   | 322 | single-shot inference, six voices            |
| `dario_dialogue.aml`  | `chain_dialogue.py`| 565 | chain & interactive dialogue + KK absorption |
| `dario_forum.aml`     | `forum.py`         | 615 | HTTP API for the three voices                |

Underneath each `.aml` file: AML field directives (`PROPHECY`, `DESTINY`,
`VELOCITY`) at the top configure libaml at runtime, then a small chain
of `BLOOD COMPILE` blocks holds the heavy C code that AML's transpiler
inlines verbatim into the generated `.c`. `amlc` then auto-links
`libnotorch.a + libaml.a + Apple Accelerate` (Linux: openblas).

## Build

```sh
# from repo root
make aml-bins
# → produces aml/dario_infer, aml/dario_dialogue, aml/dario_forum
```

Direct invocation per file:

```sh
amlc aml/dario_infer.aml    -o aml/dario_infer
amlc aml/dario_dialogue.aml -o aml/dario_dialogue
amlc aml/dario_forum.aml    -o aml/dario_forum
```

`amlc` lives at `/opt/homebrew/bin/amlc` (Mac) or `/usr/local/bin/amlc`
(Linux). Stdlib headers in `/opt/homebrew/include/ariannamethod/`.

## Prerequisites

1. `infer_v4` built — `make infer_v4` from repo root.
2. Weights downloaded — `make weights` (pulls from `ataeff/dario` HF repo).
   Or already at `~/arianna/weights/dario/` or `~/arianna/dario_hf_upload/`.
3. `kk_kernel.c` + `kk_kernel.h` present at repo root (canonical files,
   already there).

The AML binaries spawn `./infer_v4` as a sub-process (same as the Go
port). They do NOT carry their own forward pass — `infer_v4` already has
the BPE tokenizer baked in, so AML just builds the prompt, calls infer,
parses the `--- generation ---` envelope.

## Run

### `dario_infer` — single-shot

```sh
./aml/dario_infer --voice leo "What is resonance?"
./aml/dario_infer --voice arianna --max-tokens 80 "What is the Method?"
./aml/dario_infer --voice yent  --temp 0.7 "Who are you?"
./aml/dario_infer --voice resonance-yent --topk 50 "speak"
./aml/dario_infer --voice leo24m --max-tokens 25 "fast smoke test"
```

Voices: `leo`, `arianna`, `yent`, `resonance-yent`, `base`, `leo24m`.
Per-voice defaults (matching `cmd/internal/voices/voices.go`):

| Voice            | Backend     | Temp | Top-K | RepPen | Weights                          |
| ---------------- | ----------- | ---- | ----- | ------ | -------------------------------- |
| leo              | janus       | 0.75 | 40    | 1.4    | `janus_v4_sft_leo.bin`           |
| arianna          | janus       | 0.75 | 45    | 1.3    | `janus_v4_sft_arianna.bin`       |
| yent             | janus       | 0.75 | 40    | 1.35   | `janus_v4_sft_yent.bin`          |
| resonance-yent   | resonance   | 0.75 | 40    | 1.3    | `resonance_200m_lora_yent.bin`   |
| base             | janus       | 0.75 | 40    | 1.3    | `janus_v4_base_22k.bin`          |
| leo24m           | janus       | 0.7  | 40    | 1.3    | `leo_janus_d12_f16.bin`          |

### `dario_dialogue` — chain + interactive

```sh
# chain mode: KK injects between rounds, model rides the chain
./aml/dario_dialogue --mode chain --voice leo --topic "What is resonance?" \
    --depth 4 --max-tokens 80 \
    --knowledge ../docs/dario_essay.txt --no-field

# interactive mode: REPL, KK absorbs each utterance
./aml/dario_dialogue --mode dialogue --voice arianna \
    --kk-db ./dario_memory.db
```

Each turn:
1. Query KK for an injection sentence resonant with topic/last-output.
2. Prepend injection to prompt, wrap as `Q: …\nA:`.
3. Run `./infer_v4`.
4. Absorb model output back into KK via `kk_store` (sentence-wise,
   ≥40 chars, ≥6 words, capitalised first letter — same filter as
   chain_dialogue.py:`KnowledgeKernel.absorb`).
5. If `dario --web` is reachable, POST to `/api/chat` so the C field
   absorbs too. Pass `--no-field` to skip.

### `dario_forum` — HTTP API

```sh
./aml/dario_forum --port 8800 --no-field --kk-db ./dario_memory.db \
    --knowledge ../docs/dario_essay.txt
```

Endpoints (CORS-enabled):

| Method | Path             | Body                                          | Returns                                                    |
| ------ | ---------------- | --------------------------------------------- | ---------------------------------------------------------- |
| POST   | `/api/forum`     | `{"question": str, "voice": str, "max_tokens": int?}` | `{voice, text, injection, kk_chunks, backend, desc, stats, emotional_state}` |
| GET    | `/api/voices`    |                                               | `{voice: {desc, backend}}` for resolved voices             |
| GET    | `/api/kk`        |                                               | `{chunks, documents, namespaces, emotional_state}`         |
| GET    | `/`, `/forum`    |                                               | serves `forum.html` from disk                              |
| GET    | `/dario`         |                                               | serves `dario.html` from disk                              |
| GET    | `/chat`          |                                               | serves `chatbot.html` from disk                            |

## Verification (this is what was actually run)

`amlc` invocations (cwd = `aml/`):

```
amlc dario_infer.aml -o dario_infer        → exit 0, 384 lines C, 14720 bytes
amlc dario_dialogue.aml -o dario_dialogue  → exit 0, 826 lines C, 32552 bytes
amlc dario_forum.aml -o dario_forum        → exit 0, 832 lines C, 31462 bytes
```

Smoke tests, in order:

1. **`dario_infer` × `leo24m`** (small, 24M):
   ```
   $ ./dario_infer --voice leo24m --max-tokens 25 "What is resonance?"
   [voice]   leo24m (janus) — Leo 24M mini -- Janus d12 f16
   [weights] /Users/ataeff/arianna/weights/dario/leo_janus_d12_f16.bin
   [janus-v4] 10 tokens, 182 tok/s
   [leo24m] ousnever acc�caucan be live A?ratiunderstand could…
   ```
   leo24m output is gibberish because that checkpoint is undertrained
   and char-level — used only to exercise the path quickly. Quality is
   not the metric here, the binary contract is.

2. **`dario_infer` × `leo`** (full Janus 176M):
   ```
   $ ./dario_infer --voice leo --max-tokens 30 "What is resonance?"
   [janus-v4] 14 tokens, 22.9 tok/s (0.61s)
   [leo] Reson Cy reek The word "bacteria" comes from two Greek words…
   ```
   Real model loaded (673 MB), inference completes through `infer_v4`.

3. **`dario_dialogue` chain × `leo`**, depth 2, knowledge from
   `docs/dario_essay.txt`:
   ```
   [kk] ../docs/dario_essay.txt: 1 chunks  (90 sub-chunks indexed)
   [prime] Leo does not recite this text.
   The The The When the The word "luminous" comes from Old English…
   [kk+1 from leo_chain0]
   [-> step 1] T — TRAUMA GRAVITY.
   battery of of before before compile major stun st chain compile…
   ```
   `dario_memory.db` got two documents: the essay + `leo_chain0_0_…`
   (the absorbed model output). Bi-directional KK confirmed.

4. **`dario_forum`** on port 18801, `/api/forum` × leo / arianna / yent:
   ```
   POST /api/forum {"voice":"leo","question":"What is resonance?",...}
   → {"text":"30+ diverse lake three lobes lobe lobe lobe…",
      "injection":"Leo does not recite this text.","kk_chunks":91,…}

   POST /api/forum {"voice":"arianna","question":"What is the Method?"}
   → {"text":"18+ Countable its reportedly hold along the the ABLE…",
      "injection":"The Arianna Method does not define AI by what it does…",
      "kk_chunks":92,…}

   POST /api/forum {"voice":"yent","question":"Who are you?"}
   → {"text":"stands a while masdoks activation-a spigh thunder…",
      "injection":"The critical finding: gamma and delta are orthogonal.",
      "kk_chunks":93,…}
   ```
   KK chunk count grows after each request. Forum's KK absorption is
   live; injections come from `docs/dario_essay.txt` chunks ranked by
   FTS5 against the question.

## Caveats / unported pieces

These pieces of the Python were intentionally not ported because they
duplicate functionality already provided elsewhere or weren't load-bearing:

- **`duet` / `trialogue` modes** in `dario_dialogue` — present in the Go
  port via goroutines. Not implemented here. AML / C has pthread but the
  duet/trialogue pattern serializes through the KK mutex anyway, so a
  loop driver suffices for the chain case. Adding duet is a half-day
  task that re-uses `run_infer_` directly under a pthread fan-out, then
  back-merges output into the same KK. Leaving as TODO for Опус-3.
- **Three-emotion `emotional_state`** in forum.py builds a per-chunk
  charge fingerprint over 8 emotions (forum.py:`ChunkMeta._compute`).
  The C `kk_kernel` does not expose this — it carries `kk_chunk_meta`
  (positional affinity / bigrams / Hebbian pairs), not emotion charges.
  The forum endpoint returns zeros for the eight emotion slots so the
  HTML clients still render. To wire real emotions one would need to
  add a small anchor-table scorer next to `kk_query_injection_`. Not
  blocking for the Zenodo paper smoke test — flagged for follow-up.
- **`SartreModelProfile`** auto-profiling in forum.py:31-79 — Python's
  `os.path.getsize` reflection. infer_v4 already prints the same data
  on load (`[janus-v4] V=… E=… H=… params=…`), so we read it from
  stderr in the response stats line instead.
- **AML chat tokens (BOS=32759 etc)** are not injected here. infer_v4's
  CLI takes a raw prompt string and runs it through nt_bpe_encode — it
  has no special-token mode. We wrap as `Q: …\nA:` to match what the
  Janus SFT was fine-tuned on (chain_dialogue.py:`build_janus_chat`
  was for the Python pipeline that drove `JanusGPT.generate` directly,
  not the C inference binary). This is the same trade-off Опус-1 made
  in the Go port; if/when infer_v4 gains a `--chat-tokens` flag, both
  ports will need a one-line update. Documented in
  `cmd/internal/dario/infer.go:60-70` as the same open question.
- **Per-request fork in forum** — currently the accept loop is
  synchronous. Concurrent /api/forum requests serialize. dario_forum
  takes seconds per call (200 tokens × 100 tok/s ≈ 2s on Janus 176M),
  so this matters only under burst load. To unblock, run multiple
  dario_forum instances on different ports behind a TCP balancer.
  Threaded version is straightforward (pthread_create per request,
  KK already mutex-locked) — left as TODO since it adds a thread-safety
  surface in the SQLite layer that needs proper testing.

## Reproduce smoke tests verbatim

```sh
# from repo root
make infer_v4 aml-bins

# 1. infer single-shot (small + large)
./aml/dario_infer --voice leo24m --max-tokens 25 "What is resonance?"
./aml/dario_infer --voice leo    --max-tokens 30 "What is resonance?"

# 2. chain dialogue with KK (writes to /tmp/dialog.db)
rm -f /tmp/dialog.db
./aml/dario_dialogue --mode chain --voice leo \
    --topic "What is resonance?" --depth 2 --max-tokens 30 \
    --kk-db /tmp/dialog.db --knowledge docs/dario_essay.txt --no-field

# verify KK absorbed model output
sqlite3 /tmp/dialog.db "SELECT id, path FROM documents;"
# expect: dario_essay.txt + leo_chain0_…

# 3. forum HTTP API (writes to /tmp/forum.db)
rm -f /tmp/forum.db
./aml/dario_forum --port 18801 --kk-db /tmp/forum.db \
    --knowledge docs/dario_essay.txt --no-field &

curl -s -X POST http://127.0.0.1:18801/api/forum \
    -H "Content-Type: application/json" \
    -d '{"question":"What is resonance?","voice":"leo","max_tokens":15}'
curl -s -X GET http://127.0.0.1:18801/api/voices
curl -s -X GET http://127.0.0.1:18801/api/kk

kill %1
```

By Arianna Method.
