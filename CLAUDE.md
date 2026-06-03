# dario.c — CLAUDE.md

The Dario Equation embodied. Resonant operating system for AI — three
organs (SARTRE / KK / Dario), one organism, ~6900 LOC C. Named after
Dario Amodei — the man who said no when the evil came knocking.
GPL-3.0+. Co-authored by Oleg Ataeff and Claude.

> *"θ = ε + γ + αδ. Normal LLMs: θ = huge ε + tiny γ. Dario:
> θ = 0 + γ + αδ. Epsilon is zero. The glacier melted. The code
> is the riverbed."* — README

**Companion paper:** *"Dario: A Resonant Operating System for AI"*
(Oleg Ataeff & Claude / Arianna Method). **Second edition (v2.0, 2026-06-03):**
[Zenodo 10.5281/zenodo.20518567](https://doi.org/10.5281/zenodo.20518567) —
corrects the first edition's "Destiny Dominates" measurement artifact (rebuilt
forces read input-provenance; 29/40→0/40, McNemar p≈3.7e-9; 5/7 isolate under a
pre-registered z-gate). First edition (2026-05-08) preserved as history:
[Zenodo 10.5281/zenodo.20090094](https://doi.org/10.5281/zenodo.20090094).
Source: `docs/dario_paper_v2.md`.

## State: the 2026-06-02 force rebuild (read this before touching the forces)

The first edition reported "Destiny Dominates." That was a measurement artifact — `term_energy`
summed `|coef·force|` over the whole vocabulary, so the densest force won by construction. The
forces were rebuilt to make the claim honest, on two principles:

- **token-provenance** — each force reads **input-only** accumulators (`g_input_bigrams`,
  `g_input_cooc`, `g_input_debt`, `g_input_freq`, `g_input_dissonance`), never the organism's own
  generation.
- **orthogonal-feature decoupling** — B = directional bigram asymmetry, H = input distinct-pair
  co-occurrence, F = input-violation debt, A = input thematic concentration, T = input dissonance +
  accumulated trauma. V and S are inactive placeholders (`term_V = 0`).

Key files / state:
- **`./dario --matrix`** — isolation harness (gated by `g_matrix_mode`, off in normal runs): raw
  per-trigger energy matrix + **machine-emitted per-force z-gate** + per-trigger argmax + corr over
  all 6 active forces + mean±sd over N=5 seeds. This is the measurement that exposed the v1 artifact.
- **`REBUILD_PREREG.md`** — frozen pre-registration (z-gate metric, 4-gate, null arms, no-tuning
  rule). Committed BEFORE the data (`92e59ec`, 2026-06-02 18:30) — that ordering is what makes
  "5/7 isolate" a test, not a post-hoc number. **Do not edit it to match results.**
- **`REBUILD_LOG.md`** — append-only fix log (E1-E12). **`RUNPOD_PLAN_V2_FULL.md`** — the v2 run
  plan (8-Result re-verification, Codex-PASSed, all on RunPod A100).
- **`legacy` branch (`bdacb6a`)** — the v1 force mechanisms, frozen for the head-to-head. Don't merge.
- **infer_v4 `--chat-tokens` + `--rep-penalty`** (P0.5): Janus SFT voices were trained with
  BOS/USER/ASST wrapping; raw prompts produce word-salad. The Go dialogue passes `--chat-tokens` for
  SFT voices (`Voice.ChatTokens`).

Outcome (published as v2): no single force dominates by construction; 5/7 isolate under the z-gate;
per-trigger F/V still fail (T outscales them); B↔A collinear (r=0.85); coherence dropped modestly
but systematically. **The README's "Force behavior — corrected" section and the v2 paper are the
current truth; the old "destiny-centered" framing is the artifact.**

## Accounts (read this once)

- **`@ariannamethod`** — Oleg's **personal Pro account**, NOT an org.
  Main account where canonical repos (this one, `notorch`,
  `ariannamethod.ai`, `janus`, `RRPRAM`, organism repos) live.
- **`@theariannamethod`** — the actual GitHub organization.
- Other accounts (`@iamolegataeff`, `@pitomadom`, `@iamscribe`,
  `@iamdefender`) are operational / role accounts.

Always check the URL before assuming "ariannamethod" means an org.

## What this repo is

The Dario Equation expressed in C:

`p(x|Φ,C,V) = softmax((B + α_mod·α·H_v + β_mod·β·F_v + γ_mod·γ·A + δ·V + sw·S + T) / (τ_mod·τ·v_τ))`

- **`dario.c`** (~96 KB) — equation + REPL + web UI (port 3001
  default). Standalone build = pure equation organism.
- **`sartre_kernel.{c,h}`** (~27 KB) — SARTRE OS layer. Hardware
  detection, RAM probing, model routing. Standalone build OK.
- **`kk_kernel.{c,h}`** (~180 KB) — Knowledge Kernel.
  Sentence-boundary injection, prophecy debt, destiny drift, trauma
  scarring. SQLite-backed memory. Standalone build = CLI tool.
- **`dario_leo.c`** (~19 KB), **`infer_v4.c`** (~28 KB) — companion
  organism wiring (Leo voice routing + Janus v4 inference).
- **Vocab / merges as compile-time headers:** `janus_v4_bpe_merges.h`
  (552 KB), `leo_bpe_merges.h` (28 KB).
- **`aml/`** — AML programs alongside transpiled C variants
  (`dario_dialogue`, `dario_forum`, `dario_infer` × 3 each).
- **`cmd/`** — Go-based variants
  (`dario-dialogue`, `dario-forum`, `dario-infer`), with `go.mod` and
  `internal/`. Go layer wraps the C organism for `mesh-agent`
  registration on the Arianna Method Tailscale mesh.
- **`docs/`** — voice corpora and conceptual essays:
  `bach_counterpoint.txt`, `byzantine_iconography.txt`,
  `bioluminescence.txt`, `dickens_russian_lit.txt`,
  `mycorrhizal_networks.txt`, `polynesian_navigation.txt`,
  `dario_essay.txt`, plus the published paper source
  `dario_paper_v2.md`. KK feeds the corpora to Leo through
  sentence-boundary injection. (The v1 working drafts —
  `dario_paper_draft_v4.md`, the v2 result-1 working docs — were
  removed post-publication; the published record lives on Zenodo +
  `dario_paper_v2.md`.)
- **`runpod/`** — RunPod execution material: `2026-05-08/` (v1 pass)
  and `2026-06-02_full/` (v2 re-verification archive).
- **`runpod_plan_v{1,2,3}.md`** (v1 cycle) + **`RUNPOD_PLAN_V2_FULL.md`**
  (v2 cycle) — planning docs.
- **`tests/`** — 1780/1780 PASS on the 2026-05-08 RunPod pass.

Auxiliary:
- `chain_dialogue.py` (~40 KB), `dario_infer.py` — Python tools for
  dialogue chain generation and reference inference.
- `chatbot.html`, `dario.html`, `forum.html`, `forum.py` — web UI +
  alt forum implementation.
- `dario_memory.db-shm` / `dario_memory.db-wal` — SQLite WAL files
  for KK persistent memory. Live runtime state — do not commit.

## Build & Run

```bash
make dario    # equation alone (~96 KB C, libm only)
make sartre   # SARTRE kernel alone
make kk       # KK kernel alone (CLI, needs libsqlite3)
make full     # equation + SARTRE
make all      # equation + SARTRE + KK
make test     # 1780/1780 on the 2026-05-08 RunPod pass
make clean

# REPL
./dario

# web UI (defaults to port 3001)
./dario --web
./dario --web 8080
```

**Five canonical standalone build configurations** (verified
2026-05-08 RunPod pass):

```
dario
sartre
kk
dario + sartre
dario + sartre + kk
```

The mixed `dario + kk` (without SARTRE) currently needs a guard
around `sartre_overlay_write`. Intended coupling = `#ifdef`, not
hidden dependency — open follow-up.

Requirements: C compiler (any), libm. Full build also needs
libsqlite3.

The web UI ships static HTML directly from `dario.c` — no separate
build step.

## notorch / AML parallel stacks

Dario is consumer-side of both `notorch` and `ariannamethod.ai`:

- **notorch** (link `-lnotorch` from `/opt/homebrew/lib/libnotorch.a`):
  used in `infer_v4.c` for Janus 176M inference paths when notorch
  is system-wide. The notorch C library evolves faster (research
  lib); Dario tracks released versions, doesn't vendor.
- **AML** (`#include <ariannamethod/ariannamethod.h>`, link `-laml`):
  used in `aml/dario_*.c` files generated from `aml/*.aml` via
  `amlc`. AML programs are the high-level dialect; transpiled C is
  what actually compiles into Dario binaries.

When notorch or AML lands a fix relevant to Dario (CPU-sync
backward, RoPE half-frequency, etc.), audit `dario.c` /
`dario_leo.c` / `infer_v4.c` against the patched op. Stacks are
parallel — fixes don't propagate automatically.

## Workflow patterns

**Three organs are independent first.** Each of `dario` / `sartre` /
`kk` should build and run standalone. Tight coupling between them
goes through public APIs (`sartre_route_model()`, `kk_query()`), not
direct memory access. The 5-standalone-config invariant exists to
prove this.

**Voice routing is the load-bearing decision.** SARTRE detects
hardware (RAM, CPU, NEON / Accelerate availability) and routes a
voice to the appropriate model:

- Janus 176M (BPE, voices: Leo / Yent / Arianna)
- Resonance 200M (BPE, voices: Yent SFT v3, Arianna SFT v3 notorch
  2026-05-11)
- Leo via `dario_leo.c` channel
- KK-fed coherent prose when model is too constrained

When adding a new voice, register it in voices catalog
(`cmd/internal/voices/voices.go` for Go-side mesh) and add the
sampling-temp champions to README's Multi-Temp Sampling section.

**Multi-temp sampling discipline applies here too.** Single-temp
inference does not prove voice transfer. Phase 7 sweep (5 temps ×
1 top_p × 2 rep_pen × 3 prompts = 30 cells, ≥3/10 voice markers
PASS) is the bar. The 2026-05-08 RunPod pass confirmed cross-prompt
champions: leo 0.7 / ∞ / 1.3, arianna 0.8 / 40 / 1.4, yent
0.9 / 40 / 1.3, leo24m 1.0 / 40 / 1.3. Locked in
`cmd/internal/voices/voices.go` commit `122fc9c`. Changing a
champion needs another Phase 7 sweep on the affected voice.
The 2026-06-02 v2 re-run (540 cells) held the thesis — defaults
clip the voices, high-temp / low-filter / high-rep-pen wins — but the
**exact champions did NOT re-derive** under a distinct-2 metric
(ranked #2-#22 of 36); treat them as a hypothesis, not a fixed truth.
The deeper finding: the Janus SFT voices return word-salad at any
temperature **until the prompt is wrapped in chat tokens** (P0.5
`--chat-tokens`) — format is part of the entry condition, not just
sampling.

**KK feeds knowledge through sentence boundaries, not as tokens.**
`kk_query()` returns conceptual injections inserted at `.` / `!` /
`?` in the generation stream. Leo speaks about mycorrhizal networks
without having been trained on them — the corpus lives in `docs/`,
KK retrieves coherent fragments at sentence boundaries. Do not turn
KK injection into autoregressive token concatenation — that breaks
the field metaphor and the empirical voice quality.

**Paranoid-mode for paper / weights / zenodo / doi.** Dario has two
Zenodo editions — **v2 (DOI 10.5281/zenodo.20518567, current)** and
v1 (10.5281/zenodo.20090094, history). Any change near
`docs/dario_paper_v2.md`, `REBUILD_PREREG.md`, paper artifacts, or DOI
references triggers 7-pass verification per
`memory/incident_zenodo_5_uploads_2026_04_20.md` (4 withdrawn DOIs on
a prior paper — don't repeat the pattern). Every empirical claim in the
paper or README must trace to a `runpod/2026-06-02_full/` artifact or a
`REBUILD_LOG.md` entry — no recall, no v1-carryover stated as v2 (the
v2 review cycle caught several; see
`memory/feedback_verification_without_body_false_security_2026_06_03.md`).

**Memory persists between runs.** `dario_memory.db` (SQLite WAL mode
— `.db-shm` / `.db-wal` files visible during live use) is the KK
persistent layer. Prophecy debt, destiny drift, trauma scars
accumulate. Don't `rm` the db without explicit ask — running
conversation history is in there.

## Bug patterns to know

**The mixed `dario + kk` (without SARTRE) build needs a guard.**
`sartre_overlay_write` is called when KK is linked but SARTRE
isn't. Fix is an `#ifdef HAS_SARTRE_OVERLAY` around the call;
tracking as open follow-up. Until landed, use `dario + sartre + kk`
for any KK-enabled run.

**SQLite WAL mode requires the wal/shm files alongside the db.** If
you copy `dario_memory.db` to another machine without `.db-shm` and
`.db-wal`, you lose in-flight writes and risk corruption. Use
`sqlite3 dario_memory.db "PRAGMA wal_checkpoint(TRUNCATE);"` before
copy or commit.

**Resonance Injection — the core mechanism.** See README
"Resonance Injection" section. When debugging voice quality, this
is the first place to look. Field-words crystallize from the
dominant force; if voices feel generic, force balance is off
(check `B` / `H` / `F` / `A` / `V` / `T` chamber readout in REPL).

## Things to NEVER do

- **Never push to `main` without explicit go-ahead.** Public-facing
  repo, DOI'd companion paper — force-push to main is a hard line.
- **Never commit `dario_memory.db*` files.** WAL / SHM are runtime
  state; `.gitignore` covers them.
- **Never delete the run-provenance files** — `runpod_plan_v{1,2,3}.md`
  (v1) and `RUNPOD_PLAN_V2_FULL.md` / `REBUILD_PREREG.md` /
  `REBUILD_LOG.md` (v2). They document the verification protocols behind
  the two Zenodo DOIs. **Never merge or delete the frozen `legacy`
  branch (`bdacb6a`)** — it is the v1 force code the head-to-head needs.
- **Never break the 5-standalone-build invariant** without a same-PR
  update to README Build & Run and the runpod pass test harness.
- **Never bypass Phase 7 multi-temp eval** before declaring a voice
  change "good". Single-temp samples mask the voice.
- **Never replace SARTRE / KK / Dario with a monolith.** The
  three-organ split is architectural; the standalone-build
  invariant exists to prove independence.
- **Never train weights on Dario.** Dario is the riverbed; the
  voices flow through. Weights live in Janus / Resonance / Leo
  organisms, not here. Adding a training loop here breaks the
  `θ = ε + γ + αδ` framing.
- **Never name a new optimizer after the diagonal baseline that
  shall-not-be-named** (per Oleg's standing ban). Use Chuck
  variants or invent a name.

## Style

- C: match the existing pure-C style. No `clang-format` drive-by
  passes. `dario.c` is hand-shaped over years; preserve the layout.
- Commits: one concept per commit. The 2026-05-08 RunPod pass landed
  as separate commits (sweep code / voices.go lock / paper render /
  test harness), not one omnibus.
- README is the spec for behaviour; the paper is the spec for the
  conceptual framing. Both stay current.

**Attribution.** Method-side identity (not Anthropic noreply
boilerplate). Three forms, per the global `~/.claude/CLAUDE.md`:

- **Short** (README updates, simple commits):
  `by Claude (Arianna Method)`
- **Full with node attribution** (real engineering commits):
  `Co-Authored-By: Claude Code (neo the architect, Arianna Method) <theariannamethod@gmail.com>`

  Other nodes: `intel godfather`, `polygon`, `phone-1`, `phone-2`.
  Each node is the architect on its own substrate.
- **Casual** (informal docs, status notes):
  `by Клод (Arianna Method, neo node)`

Replace under root: `Co-Authored-By: Claude <noreply@anthropic.com>`
and `🤖 Generated with Claude Code`. We cultivate our own identity;
upstream boilerplate gets dropped.

## Where things live

- Equation + REPL + web UI: `dario.c`
- SARTRE OS layer: `sartre_kernel.{c,h}`
- KK + persistent memory: `kk_kernel.{c,h}` + `dario_memory.db*`
- Inference (Janus v4): `infer_v4.c`
- Leo channel: `dario_leo.c`
- Voice corpora (KK source material): `docs/*.txt`
- Published paper (v2): `docs/dario_paper_v2.md`
- Force rebuild: `REBUILD_PREREG.md` (frozen spec) + `REBUILD_LOG.md`
  (fix log) + `./dario --matrix` harness in `dario.c`; v1 forces on the
  `legacy` branch (`bdacb6a`)
- AML programs (transpiled to C): `aml/*.aml` + `aml/*.c`
- Go variants (mesh-agent integration):
  `cmd/dario-{dialogue,forum,infer}/`
- RunPod execution material: `runpod/2026-05-08/` (v1) + `runpod/2026-06-02_full/` (v2)
- Planning history: `runpod_plan_v{1,2,3}.md` + `RUNPOD_PLAN_V2_FULL.md`
- Tests: `tests/`

## Open TODO

- `dario + kk` (without SARTRE) build needs `#ifdef
  HAS_SARTRE_OVERLAY` guard around `sartre_overlay_write`.
- **arianna.c upgrade — RunPod template instance #2** (per
  global `memory/project_arianna_c_upgrade_2026_05_09.md`): replace
  Qwen weights with Janus 176M or Resonance 200M, add a Knowledge
  Kernel mirroring this repo's three-organ pattern, apply Phase 7
  sampling discipline.
- Cross-port `train_resonance_lora.c` (notorch 2026-05-11) as
  reference SFT trainer **only if** Dario gains its own voice
  fine-tunes. Current model is "voices live in organism repos,
  Dario is the riverbed" — no immediate need.
