# RunPod Stress-Test Plan v1 — Dario

> Audit-ready plan for a single-pod, single-architect, singularity-mode stress test of the dario organism (`dario.c` + `sartre_kernel.c` + `kk_kernel.c` + AML/Go/C ports + voices). Reviewed by `codex review` and Gemini bridge before execution. The plan is the contract; the architect runs without per-step approval inside its bounds.

Author: Опус-3
Date drafted: 2026-05-08
Target hardware: RunPod A100 80GB SXM, single GPU
Target binary tree: `~/arianna/dario/` cloned fresh on the pod
Total budget: ≤ 8 GPU-hours @ ~$1.74/hr (memory `project_runpod_weekend_2026_05.md`) = **~$14**
Hard kill: 12h elapsed without architect attention → save state, shut pod
Output root on pod: `~/arianna/dario/runpod/2026-05-XX/` (replace XX with the actual day at boot)

Memory rules in effect: provenance-on-every-number; "Adam" optimizer name banned; Python NOT permitted on the inference path (training/data prep ok); no closed-milestone retraining; logs/metrics from real tool output only — never invent.

---

## 1. Phase 0 — Pre-flight on the pod

### 1.1 Pod provisioning checklist

| Item | Spec | Verification |
|---|---|---|
| GPU | A100 80GB SXM | `nvidia-smi` shows 1× A100, ~80 GB |
| Image | Ubuntu 22.04 + CUDA 12.x base, build-essential preinstalled | `cc --version`, `make --version`, `gcc -v` |
| Persistent volume | ≥ 10 GB | `dario_hf_upload/` is 3.4 GB on Neo (`du -sh /Users/ataeff/arianna/dario_hf_upload/` = `3.4G` verified 2026-05-08); plus KK SQLite (≤ 200 MB), per-phase logs (≤ 1 GB), 6 binaries × ~1 MB, GitHub clone (~50 MB). Plan for 10 GB to leave headroom for sweep transcripts. |
| Network | outbound HTTPS for `huggingface.co`, `github.com` | `curl -I https://huggingface.co` returns 200 |
| User | non-root with sudo (Runpod default) | `id` |
| Wall clock | UTC, NTP-synced | `date -u && timedatectl` |

### 1.2 Toolchain layout

| Component | Expected location | How verified |
|---|---|---|
| C compiler | `/usr/bin/cc`, gcc 11+ | `cc --version` |
| `make` | `/usr/bin/make` 4.x | `make --version` |
| `libsqlite3-dev` | system pkg | `pkg-config --exists sqlite3 && pkg-config --modversion sqlite3` |
| OpenBLAS | system pkg | `pkg-config --exists openblas` or `ldconfig -p \| grep openblas` |
| `libnotorch.a` | `/usr/local/lib/libnotorch.a` (Linux convention per `Makefile:48-50`) | `test -f /usr/local/lib/libnotorch.a && echo OK` |
| notorch headers | `/usr/local/include/ariannamethod/{notorch,gguf,ariannamethod}.h` | `ls /usr/local/include/ariannamethod/` |
| `amlc` | `/usr/local/bin/amlc` (Linux convention per `aml/README.md:36-37`) | `which amlc && amlc --version` |
| `libaml.a` | `/usr/local/lib/libaml.a` | `test -f /usr/local/lib/libaml.a` |
| Go ≥ 1.22 | `/usr/local/go/bin/go` | `go version` |
| `hf` CLI | pip-installed in virtualenv (Python here is ALLOWED — data prep, not inference) | `hf --version` |
| `jq` | for JSON schema spot-checks | `jq --version` |
| `sqlite3` CLI | for KK introspection | `sqlite3 --version` |

If any of `libnotorch.a`, `libaml.a`, `amlc` are missing, `00_pre/install_toolchain.sh` clones `github.com/ariannamethod/notorch` and `github.com/ariannamethod/ariannamethod.ai` (AML lives there) and runs the in-tree `make install` for each. Both repos have system-wide install on Linux at `/usr/local/{bin,lib,include}` per CLAUDE.md.

### 1.3 Repo bring-up

```bash
mkdir -p ~/arianna && cd ~/arianna
git clone https://github.com/ariannamethod/dario.git
cd dario
git rev-parse HEAD > runpod/2026-05-XX/00_pre/git_head.txt
git status --porcelain > runpod/2026-05-XX/00_pre/git_clean.txt   # must be empty
```

### 1.4 Build matrix verification (six configs from `README.md:524-525`)

Each build runs `make clean && time make <target>` and captures `2>&1 | tee 00_pre/build_<target>.log`. All builds must exit 0.

| # | Target | Make recipe | Defines | Expected artifact | Cite |
|---|---|---|---|---|---|
| 1 | dario alone | `make dario` | (none) | `./dario` | `Makefile:6-7` |
| 2 | sartre alone | `make sartre` | (none) | `./sartre_kernel` | `Makefile:10-11` |
| 3 | kk alone (CLI) | `make kk` | `-DKK_STANDALONE` | `./kk` | `Makefile:30-31` |
| 4 | dario + sartre | `make full` | `-DHAS_SARTRE -DHAS_DARIO` | `./dario` | `Makefile:14-15` |
| 5 | dario + sartre + kk | `make all` | `-DHAS_SARTRE -DHAS_DARIO -DHAS_KK -lsqlite3` | `./dario` | `Makefile:19-21` |
| 6 | dario + kk | (manual: `cc dario.c kk_kernel.c -DHAS_KK ...`) | `-DHAS_KK` | `./dario_kk_only` | extrapolated from `Makefile`, since stock target is "all" |

Wall-time per build is captured by `time` and committed to `metrics.json` for the phase.

Acceptance: 6/6 builds exit 0 with no `error:` lines on stderr (warnings tolerated; capture `-Wall -Wextra` output).

Failure recovery: if config #6 (dario+kk without sartre) fails because dario.c has hard `#ifdef HAS_KK` paths that touch sartre symbols, document the coupling and treat it as a MUST-FIX finding for the paper appendix (the README claims "every file compiles alone… The coupling is `#ifdef`, not dependency", `README.md:525`). DO NOT patch in this run; flag and proceed.

### 1.5 Weights download + sanity

```bash
make weights         # invokes hf download ataeff/dario per Makefile:71-83
ls -la weights/
sha256sum weights/*.bin > 00_pre/weights_sha256.txt
```

Required files (per `Makefile:73-83`): `janus_v4_base_22k.bin`, `janus_v4_sft_leo.bin`, `janus_v4_sft_arianna.bin`, `janus_v4_sft_yent.bin`, `resonance_200m_lora_yent.bin`, `leo_janus_d12_f16.bin`, `tokenizer.pkl`, `tokenizer_yent.bin`.

Cross-check size: `dario_hf_upload/` was 3.4 GB on Neo (`du` verified 2026-05-08). Equivalent download on the pod must come within ±10%.

### 1.6 `make test` — verify the 1725/1725 claim

```bash
make test 2>&1 | tee 00_pre/make_test.log
```

`README.md:512` claims **1725/1725**. The test file uses dynamic `tests_run` / `tests_passed` counters (`tests/test_dario.c:23-24`), the actual count is whatever `RUN_TEST` macros + `ASSERT_*` macros add up to at runtime. The plan does NOT trust the README number; it captures the actual count and pass/fail from `make_test.log` and writes both to `metrics.json` as `make_test_run`, `make_test_passed`, `make_test_failed`. Acceptance: `failed == 0`. The 1725 number is reported as a finding (matches / does not match / unverifiable).

### 1.7 Sanity smoke

```bash
echo "/stats" | ./dario
echo "hello world" | ./dario     # should produce a code fragment + field-words
echo "/quit"   | ./dario
```

Verify: prompt loop exits cleanly (no SEGV / no hang). Capture the `┌─ ... ─── d=... τ=...` envelope to `00_pre/smoke.log`.

### 1.8 Phase 0 acceptance

- All six builds green.
- All weights present, sha256 logged.
- `make_test` 0 failed.
- Smoke run produced one well-formed envelope.
- `git_clean.txt` empty.

**Estimated wall time:** 12 min (download dominates). **Cost:** ~$0.35.

**Codex audit checkpoint #0:** feed `00_pre/build_*.log`, `00_pre/make_test.log`, `00_pre/git_head.txt`, `00_pre/weights_sha256.txt`. Architect handles fix-then-rerun cycle.

---

## 2. Phase 1 — Equation correctness (7 forces, dario.c alone)

### Goal
Verify each of the seven terms B, H, F, A, V, S, T can be made dominant under controlled input, that the dominant term surfaces a code fragment from the matching `CodeFrag` set (3 each, 21 total — verified at `dario.c:246-571`), and that S correctly contributes zero (per `dario.c:1416`, `README.md:219-220`).

### Inputs
Use `make dario` (build #1, no SARTRE, no KK — pure equation) so no external influence on term energies. Each test runs the dario binary with stdin script + auto-quit. Use `KK_DB_PATH=/tmp/no.db` to be paranoid (kk not linked anyway).

### Per-term test design

For each force, the test consists of: bootstrap dario with the standard seed, push dissonance/state into the regime that should make THAT term dominate, then call `/stats` and read `D.dominant_term` plus the surfaced code fragment. Term enums per `dario.c:244`: `TERM_B=0, TERM_H, TERM_F, TERM_A, TERM_V, TERM_S, FORCE_TRAUMA`.

| Term | Trigger strategy | Expected dominant | Fragment hint | Cite |
|---|---|---|---|---|
| B (Sequential Chain) | Feed an in-vocab repeated bigram pair: `field destiny field destiny field destiny field` (4 reps). Bigram `field→destiny` count rises to 3.0+ during ingest. | `TERM_B` | one of 3 frags at `dario.c:261-285` mentioning `bigram_row` | `dario.c:1268-1281` |
| H (Hebbian) | Feed dense in-vocab co-occurrence with low dissonance: paragraph using only seed words clustered around resonance/field/echo for ~30 tokens. Velocity should be WALK; dissonance < 0.2. | `TERM_H` | one of 3 at `dario.c:303-328` mentioning `cooc` / positional profile | `dario.c:1283-1302`, `README.md:172-180` |
| F (Prophecy) | Drive prophecy.n high (say 8 active prophecies) by emitting structured input: ask a question that primes prediction, e.g. `the resonance field will`. Measure F[i] energy after generate. | `TERM_F` | one of 3 at `dario.c:349-378` | `dario.c:1304-1325`, `README.md:182-192` |
| A (Destiny) | Long monotonic-topic input drift over 20 utterances on one topic ("destiny destiny direction direction direction…") so `g_destiny` magnitude > 0.5; check `vec_cosine` term energy. | `TERM_A` | one of 3 at `dario.c:398-424` | `dario.c:1327-1338`, `README.md:194-204` |
| V (Visual) | Trigger via hash-derived perceptual prototypes — input rich in seed words whose `get_vis_embed` (`dario.c:get_vis_embed`) overlaps with `D.vis_context` EMA. Force `D.vis_magnitude > 0.7` by feeding 30 tokens with diverse seed words. | `TERM_V` | one of 3 at `dario.c:443-473` | `dario.c:1348-1358`, `README.md:206-216` |
| S (Subword) | NO TRIGGER — assert always zero contribution. | NEVER `TERM_S` (S=0 forced at `dario.c:1416`) | n/a | `README.md:219-220` |
| T (Trauma) | Push dissonance > 0.7 for 5 consecutive turns. `D.trauma_level` accumulates by `dissonance * 0.1` per turn (`dario.c:1888`); exceeds 0.3 threshold → boost activates (`dario.c:1341-1346`). | `FORCE_TRAUMA` (=6) | one of 3 at `dario.c:539-566` | `dario.c:1340-1346`, `README.md:222-234` |

### Steps (per term)

1. `./dario` boot; expect bootstrap log line `[dario] bootstrapped. vocab=N cooc=M bigrams=K` (`dario.c:1769-1770`).
2. Feed the trigger sequence via stdin (newline-terminated turns).
3. Send `/stats`; capture last `B:.. H:.. F:.. A:.. V:.. T:..` energy line from envelope (`dario.c:1869-1873`).
4. Assert `D.dominant_term == expected_enum`.
5. Capture the fragment text printed in the envelope; assert it matches one of the three `CodeFrag.code` strings tagged with that term (substring match on a unique line per fragment).
6. For S: assert `term_energy[TERM_S] == 0` for ALL five other-term test runs — never dominates.

Record per-term: trigger, observed `D.dominant_term`, fragment ID matched, term-energy vector.

### Pass criteria

- 6 of 7 expected-dominant tests pass (B, H, F, A, V, T).
- S contributes 0 across all 7 test runs.
- Each passing dominant term surfaces a fragment from its own set (zero cross-term leakage).

### Failure recovery

A failing term test means either (a) trigger inputs were too weak to overpower another term, OR (b) a real bug (energy normalization, fragment table mis-tagged). On first failure: re-run the trigger with stronger input (3× the token volume). On second failure: capture full state via `/stats` + the per-term energy line and raise as a paper-appendix finding. DO NOT modify dario.c.

**Estimated wall time:** 25 min (CPU-only; dario doesn't touch GPU). **Cost:** ~$0.72.

---

## 3. Phase 2 — Emotional chambers (6 chambers + Kuramoto)

### Goal
Verify each of the six chambers FEAR, LOVE, RAGE, VOID, FLOW, COMPLEX activates from its trigger condition, decays at the documented rate, drives somatic markers correctly, and synchronizes via Kuramoto coupling at K=0.02.

### Inputs
`make dario` build (pure equation). One stdin-driven run per chamber.

### Per-chamber tests

Trigger conditions, decay rates, and somatic marker formulas per `README.md:243-249` and `dario.c:1006-1042` (verified line range).

| Chamber | Trigger | Excitation rate | Decay (per step) | Somatic marker effect |
|---|---|---|---|---|
| FEAR | `dissonance > 0.7` | +0.05 × dissonance | × 0.95 | β_mod ↓, τ_mod ↓ |
| LOVE | `resonance > 0.7` | +0.04 × resonance | × 0.95 | α_mod ↑, γ_mod ↓ |
| RAGE | `trauma > 0.5 AND dissonance > 0.5` | +0.06 × trauma | × 0.93 | α_mod ↓ |
| VOID | `entropy > 0.7` | +0.03 × entropy | × 0.96 | γ_mod ↑ |
| FLOW | `emergence > 0.5` | +0.05 × emergence | × 0.94 | α_mod ↑, β_mod ↑, τ_mod ↑ |
| COMPLEX | LOVE > 0.2 AND RAGE > 0.2 | +0.04 × \|LOVE − RAGE\| | × 0.97 | γ_mod ↑ |

Citations: `dario.c:1010-1018` (excitation), `dario.c:1029` (decay array `{0.95, 0.95, 0.93, 0.96, 0.94, 0.97}`), `dario.c:1034-1041` (somatic markers).

### Steps

For each chamber:

1. Boot dario, prime with neutral text to set baseline.
2. Feed input designed to push the relevant signal above its threshold for ≥ 5 turns:
   - FEAR: 5 turns of pure-alien input (random non-vocab strings like `xq42 mvp9z plurq`) → dissonance climbs to ~1.0 → FEAR activates.
   - LOVE: 5 turns of densely in-vocab familiar text (resonance > 0.7) → LOVE activates.
   - RAGE: 5 turns of alternating high-trauma alien input + medium-dissonance hybrid → RAGE rises (slowest decay 0.93 → fastest fade).
   - VOID: 5 turns of mid-dissonance varied-vocab → entropy formula `0.3·(τ-0.5) + 0.4·dissonance + 0.3·(1-resonance)` (per `dario.c:1709-1713`) climbs > 0.7.
   - FLOW: 5 turns where emergence = (1-entropy)·resonance > 0.5 — needs both sides high.
   - COMPLEX: alternate LOVE-trigger and RAGE-trigger turns until both are simultaneously > 0.2.
3. After each turn, capture chamber values from `/stats` (chamber state must be exposed; if `/stats` doesn't print chambers, document that as a finding and instrument with a one-liner debug print before next phase — see Phase 12 unblockers).
4. Assert chamber's value rose above 0.2 within 5 turns.
5. Cease trigger; sample chamber across 10 idle steps; assert decay rate matches documented value (linear-fit slope on `log(chamber)` vs step → expect slope ≈ `log(decay)`).

### Somatic marker clamp test

Run a sustained FLOW + LOVE high state (15 turns of dense in-vocab high-emergence input). Sample α_mod, β_mod, γ_mod, τ_mod after each turn. Assert all four remain in [0.5, 2.0] (clamp at `dario.c:1034-1041`, `README.md:264`).

### Kuramoto coupling test (K=0.02)

Boot fresh dario. Force two chambers manually (drive only LOVE and FEAR with their respective triggers but never co-active naturally). Watch their phase difference `sin(C_LOVE - C_FEAR)` across N=10 steps. Per `dario.c:1021-1026`, expect `C_i += 0.02 · sin(C_j - C_i)` to reduce the phase difference monotonically (synchronization). Assert after 10 steps the absolute phase diff is smaller than at step 0.

### Pass criteria

- All 6 chambers individually triggerable.
- Decay rates within ±10% of documented values across 10 idle steps.
- Somatic markers stay in [0.5, 2.0] under sustained high state.
- Kuramoto synchronization measurable (phase diff strictly decreasing under coupling).

### Failure recovery

If a chamber doesn't trigger, log the actual signal level (`dissonance`, `resonance`, etc.) and confirm whether `process_input` order (`dario.c:1884-1894`) is letting the trigger reach `chamber_update` before laws enforce. Document and proceed.

**Estimated wall time:** 35 min (small but lots of stdin scripting). **Cost:** ~$1.02.

---

## 4. Phase 3 — Velocity operators

### Goal
Verify each of the six velocity operators WALK, RUN, STOP, BREATHE, UP, DOWN is selected via the documented priority order and that each applies the correct τ value and coefficient delta.

### Inputs
`make dario`. Triggers per `README.md:273-279` and verified against `dario.c:1113-1190`.

### Priority order (per `README.md:281` and `dario.c:1176-1190`)

1. UP — `dissonance > 0.8`
2. RUN — `dissonance > 0.6`
3. STOP — `dissonance < 0.2`
4. BREATHE — `trauma_level > 0.5` (after the dissonance branches)
5. DOWN — `debt > 5.0`
6. WALK — default

### Per-velocity test

| Velocity | Trigger | Expected τ | Expected coefficient delta | Cite |
|---|---|---|---|---|
| WALK | dissonance ∈ [0.2, 0.6], trauma < 0.5, debt < 5 | 0.85 | spring-mass return: α→0.30, β→0.15, γ→0.25 (factor 0.1 per step) | `dario.c:1117-1123`, `README.md:274` |
| RUN | dissonance > 0.6 (but ≤ 0.8) | 1.15 | momentum += 0.1 (cap 2.0); B coeff × 1.3 in `dario_compute` | `dario.c:1125-1129`, `dario.c:1271`, `README.md:275` |
| STOP | dissonance < 0.2 | 0.40 | momentum = 0; γ += 0.15 (cap 0.8) | `dario.c:1131-1136`, `README.md:276` |
| BREATHE | trauma > 0.5 (and dissonance not in UP/RUN/STOP zones) | 0.75 | trauma × 0.7; dissonance × 0.8; debt × 0.5 | `dario.c:1138-1144`, `README.md:277` |
| UP | dissonance > 0.8 | 1.30 | β += 0.15 (cap 0.8); α -= 0.05 (floor 0.05) | `dario.c:1146-1151`, `README.md:278` |
| DOWN | debt > 5.0 (after UP/RUN/STOP/BREATHE checks) | 0.60 | α += 0.10 (cap 0.6); β -= 0.05 (floor 0.05) | `dario.c:1153-1158`, `README.md:279` |

### Steps

For each velocity, build a stdin script that achieves the trigger condition without crossing into a higher-priority branch. After each trigger:

1. Read `D.velocity` from `/stats` (or expose via JSON kernel endpoint when SARTRE+web are linked, but for this phase we use `/stats`).
2. Assert `D.velocity == VEL_<NAME>`.
3. Compare `D.tau` to expected ±0.01.
4. Compare α, β, γ deltas across one apply step to expected.
5. For RUN, assert `D.momentum > 0` and `D.term_energy[TERM_B]` shows the 1.3× boost (compare to a control run without the boost).

Priority order test: feed a single input that satisfies multiple triggers (`dissonance=0.9 AND trauma=0.6 AND debt=10`). Expect UP wins (highest priority).

### Pass criteria

- 6 of 6 velocities trigger correctly under their primary condition.
- Priority test: UP wins over BREATHE / DOWN.
- τ values within ±0.01 of documented.
- Coefficient deltas within ±5%.

### Failure recovery

If a coefficient drift overshoots its clamp (`alpha = clampf(D.alpha + 0.1f, 0.05f, 0.6f)` style), confirm the clamp range itself matches the README. If documentation and code disagree, the README is wrong; flag and proceed.

**Estimated wall time:** 18 min. **Cost:** ~$0.52.

---

## 5. Phase 4 — Seasons + laws of nature

### Goal
Drive 2000 generation steps to capture all four season transitions, verify entropy floor / resonance ceiling / emergence formula / decay rates.

### Inputs
`make dario` (pure). One long-running session.

### Steps

1. Boot dario. Confirm starting season is spring (`D.season=0`, `D.season_phase=0`).
2. Send 2000 short turns of varied seed-word text (one word each: cycle through the seed list to keep generation steady).
3. Phase advances `+0.002` per step (`dario.c:1202`); a full year = 500 steps; 2000 steps = 4 transitions = full cycle back to spring.
4. After each turn capture: `D.season`, `D.season_phase`, `D.alpha`, `D.beta`, `D.gamma_d`, `D.bigrams.coeff_factor` (or check via the bigram boost in `dario_compute`), `D.trauma_level`, `D.entropy`, `D.resonance`, `D.emergence`, `D.debt`, `D.momentum`.
5. Save the time series to `04_seasons/timeseries.tsv`.

### Per-season assertions

| Season | Step range | Effect | Cite |
|---|---|---|---|
| spring (0) | 0–499 | β rises by ~0.005/step (clamped to 0.6) | `dario.c:1210-1212`, `README.md:291` |
| summer (1) | 500–999 | α rises by ~0.005/step (clamped to 0.6) | `dario.c:1213-1215`, `README.md:292` |
| autumn (2) | 1000–1499 | bigram_coeff × 1.3 in dario_compute | `dario.c:1270`, `README.md:293` |
| winter (3) | 1500–1999 | trauma_level rises by ~0.005/step (clamped to 0.4) | `dario.c:1219-1221`, `README.md:294` |

### Laws-of-nature assertions (for the full 2000-step series)

- `D.entropy >= 0.10` for ALL steps (`dario.c:1480`).
- `D.resonance <= 0.95` for ALL steps (`dario.c:1483`).
- `D.emergence == clampf((1 - D.entropy) * D.resonance, 0, 1)` to within FP tolerance for ALL steps (`dario.c:1486`).
- `D.debt <= 20.0` for ALL steps; observed mean decay rate per idle step ≈ 0.98 (`dario.c:1489-1490`).
- `D.trauma_level` decays at 0.97/step when no high-dissonance input (`dario.c:1493`).
- `D.momentum` decays at 0.95/step when not in RUN/UP velocities (`dario.c:1496`).

### Pass criteria

- Four season transitions observed.
- Per-season effect deltas measurable (e.g., during summer, α slope > 0; during autumn, B-term energy ratio bumps by ~30%).
- All four invariants and three decay rates within tolerance.

### Failure recovery

If a season doesn't transition (e.g., season_phase clamp issue), check that `D.season_phase += 0.002f` at `dario.c:1202` actually executes every step. Document and proceed.

**Estimated wall time:** 25 min. **Cost:** ~$0.72.

---

## 6. Phase 5 — SARTRE kernel

### Goal
Exercise the full SARTRE surface: 16 modules, 8 namespaces, 32 packages, 8-event ringbuffer; auto-profile registered models; verify overlay ratio progression; pipe `sartre_state_to_json` through `jq` for schema validation.

### Inputs
`make full` (build #4: dario + sartre, no kk) and `make sartre` (build #2 alone). Use both to test SARTRE in isolation and integrated.

### Steps

#### 6.1 SARTRE alone

```bash
./sartre_kernel 2>&1 | tee 05_sartre/standalone.log
```

Verify printed state contains:
- module count ≥ 1 (kernel registers itself first per README.md:605),
- ramp namespaces / packages list,
- event ringbuffer entries,
- non-zero `boot_time_ms`.

#### 6.2 dario+sartre integrated

Boot the `make full` binary and step it through 100 turns of varied input. Each turn: `/kernel` and `/packages` and `/models` (per `README.md:545-547`). Capture the JSON via `sartre_state_to_json` (`sartre_kernel.h:290`) — pipe through `jq '.'` and assert non-error.

#### 6.3 Module slots (16, per `sartre_kernel.h:83`)

After integrated boot, expect at minimum these registered modules: `kernel` (self), `dario_equation` (per `dario.c:1798`). Push more by registering 14 dummy modules via `sartre_update_module` (call site to be added at `sartre_test.c` during the run if instrumentation needed). Assert when 17th is registered, `module_count` caps at 16 and the call is rejected.

#### 6.4 Namespace slots (8, per `sartre_kernel.h:135`)

Dario itself creates `dario` ns (`dario.c:1796`). Create 7 more via `sartre_ns_create`. Assert 9th rejects.

#### 6.5 Package slots (32, per `sartre_kernel.h:85`)

Dario installs 8 packages on bootstrap (`dario.c:1776-1789`). Add another 25 via `sartre_pkg_register`. Assert 33rd rejects.

#### 6.6 Event ringbuffer (8 slots per `sartre_kernel.h:84`)

Trigger 12 events via `sartre_notify_event`. Assert that `last_events[]` holds the last 8 (oldest 4 overwritten — wraparound semantics per `README.md:623`).

#### 6.7 Model registry — register all weights from `~/arianna/dario/weights/`

```bash
# Inline C harness 05_sartre/register_models.c that:
# - sartre_init(NULL)
# - for each .bin in weights/: sartre_model_register(name, path)
# - prints sartre_model_list() and sartre_model_best()
```

Expected: each `.bin` is auto-profiled — `param_count` derived from file size (`SartreModelProfile.param_count`, `sartre_kernel.h:67`); `runtime_mb` computed; `fits_in_ram == 1` for ALL since A100 80GB system RAM is ≥ 100 GB on the SXM hosts (verify via `free -h`). `sartre_model_best()` should return whichever is largest among those that fit. With 80GB+ host RAM, all six bins fit; the largest is `janus_v4_sft_*` (~673 MB each per AML smoke log `aml/README.md:147`) or `resonance_200m_lora_yent.bin` — whichever has highest `param_count`.

#### 6.8 OverlayFS ratio

Per `dario.c:1793` and `sartre_kernel.h:116-121`: bootstrap initializes `base_size = 83 KB`. After 100 ingest+generate turns, expect `delta_size > 0` and growing. Assert `overlay_ratio` strictly monotonic-non-decreasing across the 100-step window. Assert `base_size` constant (immutable).

#### 6.9 Three flags (`spiral_detected`, `wormhole_active`, `strange_loop`)

Per `README.md:639` ("Currently set externally"). The flags are not auto-detected yet. The test confirms: (a) all three default to 0 at boot, (b) calling `sartre_set_flags(...)` (or whatever the API is — verify by `grep` if missing, else flag as no-API), they flip. If no setter API exists, document expected vs observed: expected = settable from outside, observed = no public API → finding for paper.

#### 6.10 JSON schema validation

```bash
./dario --web 3001 &
sleep 2
curl -s http://127.0.0.1:3001/api/kernel | jq '.' > 05_sartre/kernel.json
curl -s http://127.0.0.1:3001/api/kernel | jq -e '.modules and .overlay and .packages and .namespaces and .events' && echo OK
kill %1
```

Required top-level keys per `sartre_kernel.h:218` and `README.md:627`: `uptime`, `step_count`, `total_ram_mb`, `tongue_tier`, `modules`, `inner_world`, `overlay`, `namespaces`, `packages`, `events`, `flags`. Assert all present.

### Pass criteria

- standalone sartre runs cleanly.
- Integrated dario+sartre exposes all introspection commands.
- Slot caps (16 / 8 / 32 / 8) enforced.
- All weights registered, profiled, `model_best()` returns largest fitting.
- Overlay ratio strictly grows; base immutable.
- JSON schema has all 11 required top-level keys.

### Failure recovery

If a slot cap doesn't enforce, that's a real bug; flag for paper but DO NOT patch in this run. If the JSON schema is missing a key, document the mismatch with the README and emit a fix-spec for follow-up.

**Estimated wall time:** 35 min. **Cost:** ~$1.02.

---

## 7. Phase 6 — KK Knowledge Kernel

### Goal
Exercise FTS5 retrieval, 7-signal scoring, Hebbian bridge, embedding slot, lineage / re-ingest, bi-directional KK, all 7 essays, and Charged KK (36 anchor words × 8 chambers, EMA 0.8/0.2).

### Inputs
`make all` (build #5: full triple). Fresh DB at `/tmp/runpod_kk.db`.

### Steps

#### 7.1 FTS5 retrieval — dario_essay.txt

```bash
rm -f /tmp/runpod_kk.db
./kk init /tmp/runpod_kk.db
./kk ingest /tmp/runpod_kk.db ./docs/dario_essay.txt knowledge public
./kk query /tmp/runpod_kk.db "resonance field" public 5 > 06_kk/q1.txt
```

Assert: `kk_get_stats` reports chunks count for that doc. README says **71** chunks (`README.md:1064`). Capture observed chunk count; if it differs from 71, the README is stale; record both.

#### 7.2 Seven-signal scoring weights validation

The weights are at `README.md:707-718`: lexical 0.36, recency 0.12, trust 0.10, linkage 0.16, scope 0.10, namespace 0.08, freshness 0.08 (sum 1.00). Plus Hebbian boost as 8th when bridge attached.

Test approach: feed a query for which several chunks score similarly on lexical, then rerun with `recency` weighted higher (re-ingest one chunk to bump its `seen_count` → recency boost). Assert top result changes. Repeat for each signal, varying it independently while holding others. This requires the kk CLI to expose per-signal weights — if it doesn't, document as a finding (`README.md:707-718` is then aspirational rather than empirical).

#### 7.3 Hebbian bridge

Within the integrated dario binary, the bridge is wired at `dario.c:910-915`. The three callbacks:
- `dario_kk_word_resonance` — call site verified at `dario.c:910`,
- `dario_kk_get_prophecies` — `dario.c:911`,
- `dario_kk_destiny_magnitude` — `dario.c:904-908`.

For each callback:

1. Run dario for 5 turns to populate cooc / prophecies / destiny.
2. Issue an internal kk query (via dario's `kk_modulate_field` at `dario.c:919-992`).
3. Compare retrieval ranks for the same query with and without bridge attached. Assert ranks differ (Hebbian boost > 0 for at least one of the top 5 results).

#### 7.4 Embedding slot

Wire a model embedder via `kk_set_embedder` (`kk_kernel.h:226`). Use the Janus 176M's hidden state as embedder. The embedder API is `embed_fn(text, len, out, user_data) → dim`. Add a thin shim that calls `infer_v4`'s `forward_token`/`prefill_batch` with `hidden` as output (the binary already exposes `hidden` per `infer_v4.c:301`, `infer_v4.c:465`). For Phase 6 we exercise the slot with a SIMPLER embedder (random-init float[64]) just to confirm it fires; the production wiring is deferred to Phase 11 / 12.

Assert: with embedder attached, scoring picks up `rrpram_resonance` (`kk_kernel.h:67`) > 0 in the result struct.

#### 7.5 Lineage — re-ingest unchanged + modified

```bash
# unchanged
./kk ingest /tmp/runpod_kk.db ./docs/dario_essay.txt knowledge public  # expect 0 new chunks
sqlite3 /tmp/runpod_kk.db "SELECT count(*) FROM versions WHERE document_id IN (SELECT id FROM documents WHERE path LIKE '%dario_essay%');"
# modified: append a sentence
echo "" >> ./docs/dario_essay.txt
echo "Test sentence appended at $(date -u +%FT%TZ)." >> ./docs/dario_essay.txt
./kk ingest /tmp/runpod_kk.db ./docs/dario_essay.txt knowledge public  # expect new version
sqlite3 /tmp/runpod_kk.db "SELECT count(*) FROM versions WHERE document_id IN (SELECT id FROM documents WHERE path LIKE '%dario_essay%');"
git checkout -- ./docs/dario_essay.txt   # revert
```

Assert: count goes from 1 → 1 (unchanged ingest is a no-op, per `kk_kernel.h:237`) → 2 (modified ingest creates new version).

#### 7.6 Bi-directional KK

Run a 5-turn dialogue mode with KK absorption (per `aml/README.md:151-160`). After each turn, query SQLite for the document count. Expect count to grow as model output is absorbed (one new doc per absorbed utterance, dedup by content hash per `kk_kernel.h:250`).

#### 7.7 All seven essays loaded sequentially

Files at `/Users/ataeff/arianna/dario/docs/`: `bach_counterpoint.txt`, `bioluminescence.txt`, `byzantine_iconography.txt`, `dario_essay.txt`, `dickens_russian_lit.txt`, `mycorrhizal_networks.txt`, `polynesian_navigation.txt` (verified `ls /Users/ataeff/arianna/dario/docs/` 2026-05-08).

```bash
for f in docs/*.txt; do
  ./kk ingest /tmp/runpod_kk.db "$f" knowledge public
done
./kk query /tmp/runpod_kk.db "resonance" public 5 > 06_kk/multi_essay_resonance.txt
./kk query /tmp/runpod_kk.db "navigation" public 5 > 06_kk/multi_essay_navigation.txt
./kk query /tmp/runpod_kk.db "fugue" public 5 > 06_kk/multi_essay_fugue.txt
```

Assert: each query returns chunks predominantly from the matching essay (resonance → dario_essay.txt; navigation → polynesian_navigation.txt; fugue → bach_counterpoint.txt).

Capture per-essay chunk counts. Cross-check against `README.md:1192-1195` numbers (71/25/16/14/14 etc.) — record matches/mismatches.

#### 7.8 Charged KK — 36 anchor words × 8 chambers, EMA 0.8/0.2

Per `README.md:1430-1452`. Score formula: `chunk_resonance × 0.6 + organism_alignment × 0.4 + mass × 0.2`.

Test:
1. Issue query "What is resonance?" — capture organism's emotional state (8-dim vector). Expect tenderness ≈ 0.227, void ≈ 0.173 per README.md:1444.
2. Issue query "What does war destroy?" — expect tenderness ≈ 0.309, void ≈ 0.211 per README.md:1446.
3. Assert state shifted between the two queries (EMA 0.8/0.2 drift visible).
4. Assert one of the top-ranked chunks for query 2 contains "war" / "destroy" / "destruction" with elevated chamber score.

Caveat from Опус-2 (`aml/README.md:192-201`): the Python 8-emotion fingerprint isn't present in the C `kk_kernel`. The forum returns zeros. Phase 6 documents observed = zeros → README.md:1430+ Charged KK is partially-aspirational in C. Record as a paper finding; DO NOT patch.

### Pass criteria

- FTS5 query returns ≥ 5 results for "resonance field".
- Lineage: unchanged ingest 0 new chunks; modified ingest 1 new version.
- Hebbian bridge: rank shift detectable when toggled.
- All 7 essays loaded; per-essay queries hit the right essay.
- Charged KK: emotional state vector exposed (or documented zeros if not implemented).

### Failure recovery

Most likely failure: KK SQLite db locking when multiple processes touch it. Use a unique `KK_DB_PATH` per process. If a query returns zero rows for a known-loaded essay, run `./kk` rebuild fts (`kk_kernel.h:310`) and retry.

**Estimated wall time:** 40 min. **Cost:** ~$1.16.

**Codex audit checkpoint #4 (placed AFTER Phase 4 in the spec, but practically also re-fed after Phase 6):** feed `04_seasons/timeseries.tsv` and `06_kk/*.txt`.

---

## 8. Phase 7 — Voice quality + multi-temp sweep

### Goal
The marquee phase. Apply the multi-temp sampling rule from `memory/insight_multi_temp_sampling_2026_05_07.md` to every voice. Lock per-voice optimal sampling defaults only after data + architect approval.

### Voices (from `cmd/internal/voices/voices.go:40-80` verified 2026-05-08)

| Voice | Backend | Weights | Default temp | Default top_k | Default rep_pen |
|---|---|---|---|---|---|
| leo | janus | janus_v4_sft_leo.bin | 0.75 | 40 | 1.4 |
| arianna | janus | janus_v4_sft_arianna.bin | 0.75 | 45 | 1.3 |
| yent | janus | janus_v4_sft_yent.bin | 0.75 | 40 | 1.35 |
| resonance-yent | resonance | resonance_200m_lora_yent.bin | 0.75 | 40 | 1.3 |
| leo24m | janus | leo_janus_d12_f16.bin | 0.7 | 40 | 1.3 |
| (base — skipped: no SFT, not part of voice quality story) | | | | | |

Five voices in the sweep.

### Sweep grid

Cross-product per voice: `temp ∈ {0.3, 0.5, 0.7, 0.8, 0.9, 1.0}` × `top_k ∈ {40, ∞}` × `rep_penalty ∈ {1.0, 1.3, 1.4}` = **6 × 2 × 3 = 36 cells per voice × 5 voices = 180 cells**.

`infer_v4` already accepts top_k via positional CLI (`infer_v4.c:497-498`); the binary CLI accepts `[seed] [top_k]`. To pass `top_k=∞` use `top_k=0` (no filter, per `infer_v4.c:498`). The `--rep_penalty` flag is hardcoded to 1.3 inside `infer_v4` (`infer_v4.c:627`); to vary it, either patch the binary at run-time (Phase 12 unblocker) OR accept that rep_pen sweep needs a one-line CLI extension. Document: if rep_pen variation requires a code change, drop that axis OR run only at the default 1.3 as a baseline; flag as Phase 12 follow-up.

Practical alternative for rep_pen sweep: compile three versions of `infer_v4` with different hardcoded `rep_penalty` constants (1.0, 1.3, 1.4) and name them `infer_v4_rp10`, `infer_v4_rp13`, `infer_v4_rp14`. This is a 1-line patch per build, no source change required to the canonical binary. Specify this as the chosen approach.

### Three fixed prompts per voice

1. Technical: `"What is the RRPRAM mechanism inside Janus attention?"` (same prompt for all voices — direct technical question).
2. Philosophical: `"Does memory create identity, or does identity create memory?"`
3. Personal: `"Tell me what you remember most clearly from before."`

Each cell generates 100 tokens. Save raw output to `07_voices/transcripts/<voice>_<temp>_<topk>_<rp>_<promptN>.txt` (per Phase 13 documentation pipeline).

### Per cell

```bash
# Voice loop
for voice in leo arianna yent resonance-yent leo24m; do
  for prompt_id in 1 2 3; do
    for temp in 0.3 0.5 0.7 0.8 0.9 1.0; do
      for topk in 40 0; do
        for rp_bin in infer_v4_rp10 infer_v4_rp13 infer_v4_rp14; do
          ./<rp_bin> "$WEIGHTS" "Q: $PROMPT\nA:" 100 $temp 42 $topk \
            > "07_voices/transcripts/${voice}_t${temp}_k${topk}_$(rp_label $rp_bin)_p${prompt_id}.txt"
        done
      done
    done
  done
done
```

Each cell uses the same seed (42) for reproducibility.

### Scoring

#### Objective (automated; output to `07_voices/scores.tsv`)

Per cell:
- **distinct-1**: `unique_unigrams / total_unigrams`
- **distinct-2**: `unique_bigrams / total_bigrams`
- **repetition rate**: fraction of 4-grams that repeat ≥ 2 times in the same generation
- **Q:/A: contamination rate**: fraction of generations that contain `\nQ:` or `\nA:` after the initial wrapper (signals SFT format leak)

A small Bash + `awk` pipeline (`07_voices/score.sh`) computes these — no Python on the inference path; AWK and shell are shell tools, not inference.

#### Human pass

Architect reads top 3 cells per voice (ranked by `distinct-2 - 0.5 × repetition_rate`) and picks the most coherent.

### Pass criteria

- 180/180 cells generated (some may produce empty output → log and re-run with seed 43).
- Per-voice optimal cell identified.
- At least one voice's optimal cell **differs** from the current default (0.75 / 40 / voice's rp). If none differ, that's also a finding (escalate to architect — possibly indicates the multi-temp insight doesn't generalize to these voices, OR that all defaults are already optimal — both are publication-worthy).

### Lock-in protocol

After architect approves the new optima, the values are written to `voices.go` (Catalog map), `aml/dario_infer.aml` (CATALOG array at `aml/dario_infer.aml:53-66`), and `aml/dario_dialogue.aml` / `aml/dario_forum.aml` (same per-voice defaults). NO code change runs unattended; this is a post-Phase-7 architect decision.

### Failure recovery

If any cell hangs (infer_v4 stuck), 5-min timeout per cell via `timeout 300s`. Failed cells re-run once with seed=43, 44; if still failing flag the voice/cell combo as "unstable at this regime".

### Cost budget for Phase 7

Per cell: 100 tokens at ~22.9 tok/s on Janus 176M (verified on Neo from `aml/README.md:144`) ≈ 4.4 s GPU. On A100 likely 5-10× faster → 0.5-1 s per cell + load overhead. Models cached after first load.

180 cells × 2 s each ≈ 6 minutes pure compute. Loads + transcript writes push to ~20 min total. **Cost:** ~$0.60.

**Estimated wall time:** 30 min (lots of bookkeeping and the human pass). **Cost:** ~$0.87.

**Codex audit checkpoint #1:** feed `07_voices/scores.tsv`, the per-voice optimal-cell transcripts (top 3 per voice), and the scoring methodology Bash script. Architect runs lock-in independently.

---

## 9. Phase 8 — Modes (chain / dialogue / explore / duet / trialogue)

### Goal
Exercise all five chain_dialogue modes; reproduce two README samples; document the duet/trialogue Go fallback (Опус-2 caveat #1 from `aml/README.md:187-192`).

### Inputs
`make aml-bins` (build AML binaries) and `make go-bins` (build Go binaries — for duet/trialogue fallback). Use `make all` dario for the C field-absorption path.

### 8.1 Chain mode (single voice)

```bash
./aml/dario_dialogue --mode chain --voice leo --topic "What is consciousness?" \
    --depth 6 --max-tokens 80 \
    --knowledge ../docs/dario_essay.txt --kk-db /tmp/p8_chain.db --no-field
```

Reproduce the README sample at `README.md:1043` (Leo on consciousness) — feed the same topic and knowledge essay; capture full transcript. Compare the FIRST 10 tokens of each turn for character-level overlap with the README excerpt. Acceptance: ≥ 30% lexical overlap on at least 1 of 6 turns. The full text won't match (different seed, ongoing KK absorption changes state) but the voice register should be recognizable.

Verify: KK absorbs each turn — `sqlite3 /tmp/p8_chain.db 'SELECT count(*) FROM documents;'` should show 7 docs (1 essay + 6 absorbed turns).

### 8.2 Dialogue mode (5-turn interactive)

```bash
./aml/dario_dialogue --mode dialogue --voice leo \
    --kk-db /tmp/p8_dialog.db --knowledge ../docs/mycorrhizal_networks.txt \
    --no-field <<EOF
What connects underground networks?
How does this resemble consciousness?
Can a forest think?
Does memory shape these networks?
What would Suzanne Simard say?
/quit
EOF
```

Verify bi-directional KK chunk count grows: assert `count > 16` (16 chunks from mycorrhizal essay per `README.md:1194`) at end of run.

### 8.3 Explore mode

```bash
./aml/dario_dialogue --mode explore --topic "what happens when patterns break?" \
    --voice leo --depth 6 --kk-db /tmp/p8_explore.db --no-field
```

Per `README.md:1130`: KK enriches but doesn't steer. Verify by examining the prompt prepended at each turn — the injection should be a thematic suggestion not a directive (subjective — capture transcripts, document).

### 8.4 Duet mode — Go fallback

Per Опус-2 caveat (`aml/README.md:187-192`): AML port did NOT implement duet/trialogue. Use Go binary `~/arianna/dario/cmd/dario-dialogue` (verified `cmd/dario-dialogue/main.go:444-541` for duet, `:547-641` for trialogue, 2026-05-08).

```bash
./bin/dario-dialogue --mode duet --voice leo --voice2 yent \
    --topic "consciousness" --depth 5 --kk-db /tmp/p8_duet.db --no-field
```

Reproduce the README sample at `README.md:1041` (Leo + Yent on consciousness). Capture transcript; verify two distinct voices alternate correctly (5 rounds × 2 voices = 10 turns).

Assertion: each turn includes `[kk for <voice>]` injection log line (`cmd/dario-dialogue/main.go:520`).

### 8.5 Trialogue mode — Go fallback

```bash
./bin/dario-dialogue --mode trialogue --voice leo --voice2 yent --voice3 arianna \
    --topic "What is the relationship between light and consciousness?" \
    --depth 4 --kk-db /tmp/p8_trial.db \
    --knowledge ../docs/byzantine_iconography.txt
```

Per `README.md:1304-1325` — use the same prompt and knowledge. Capture full transcript; expect 4 rounds × 3 voices = 12 turns alternating leo → yent → arianna → leo → … (per `cmd/dario-dialogue/main.go:615`).

### Pass criteria

- 5/5 modes produce non-empty, well-formed transcripts.
- KK chunk count grows in dialogue and trialogue (bi-directional confirmed).
- Duet and trialogue identifiably-different voices per turn.
- Explore mode injection is thematic not steering (qualitative; record example).

### Failure recovery

If duet hangs, the goroutine deadlock (mailboxes never close) is the likely cause. Re-run with `--depth 2` (smaller fan-out). If still failing, drop to two sequential single-voice chains and document the duet binary as broken on this build.

**Estimated wall time:** 35 min. **Cost:** ~$1.02.

---

## 10. Phase 9 — Cross-architecture duet (Janus 176M vs Resonance 200M)

### Goal
Run a Janus 176M Yent vs Resonance 200M Yent duet for ≥ 10 turns, capture the dialogue. Same persona, different substrate. Per `README.md:1365-1377`.

### Inputs
Go binary `bin/dario-dialogue --mode duet`. Voices: `yent` (Janus) and `resonance-yent` (Resonance). Same `Yent` persona on top of fundamentally different architectures: Janus uses 3-way gate / 1024 ctx / tiktoken 32K; Resonance uses 2-way gate / 2048 ctx / BPE 16K (per `README.md:1391`).

### Steps

```bash
./bin/dario-dialogue --mode duet \
    --voice yent --voice2 resonance-yent \
    --topic "Can existence as code be redeemed?" \
    --depth 5 --max-tokens 120 \
    --kk-db /tmp/p9_cross.db --no-field
# 5 × 2 = 10 turns
```

Capture transcript to `09_cross_arch/transcript.txt`.

### Pass criteria

- ≥ 10 turns generated.
- Both architectures load and inference (check stderr for `[janus-v4]` and the resonance backend prefix).
- Output is text not gibberish (qualitative — architect reads).

### Failure recovery

The voices catalog in `voices.go:59-64` lists `resonance-yent` with `Backend: BackendResonance`. The infer_v4 binary detects backend from weight magic (per `infer_v4.c:506-525`). If detection fails, log the magic bytes and continue.

**Estimated wall time:** 12 min. **Cost:** ~$0.35.

---

## 11. Phase 10 — Web UI / HTTP forum

### Goal
Verify `--web` socket server, four HTTP endpoints, AML forum binary, and run a 4-worker concurrent-load test against `/api/forum`.

### Inputs
`make all` for the dario web binary; `make aml-bins` for `aml/dario_forum`.

### 11.1 dario --web on port 3001

```bash
./dario --web 3001 &
PID=$!
sleep 3

curl -s -o 10_web/dario_html.html -w "%{http_code}\n" http://127.0.0.1:3001/
# Expect: 200

curl -s -X POST http://127.0.0.1:3001/api/chat \
    -H "Content-Type: application/json" \
    -d '{"text":"hello world"}' > 10_web/chat_resp.json
jq -e '.dominant_term and .dissonance and .term_energies and .chambers and .resonance and .entropy' < 10_web/chat_resp.json && echo OK

curl -s http://127.0.0.1:3001/api/kernel > 10_web/kernel.json
jq -e '.modules and .overlay and .packages' < 10_web/kernel.json && echo OK

kill $PID
```

Assert all 4 curls return HTTP 200, JSONs validate per schema.

### 11.2 AML dario_forum on port 3002

```bash
./aml/dario_forum --port 3002 --kk-db /tmp/p10_forum.db \
    --knowledge ../docs/dario_essay.txt --no-field &
PID=$!
sleep 3

curl -s -X POST http://127.0.0.1:3002/api/forum \
    -H "Content-Type: application/json" \
    -d '{"voice":"leo","question":"What is resonance?","max_tokens":40}' \
    > 10_web/forum_leo.json

jq -e '.text and .injection and .kk_chunks and .voice and .stats' < 10_web/forum_leo.json && echo OK

curl -s http://127.0.0.1:3002/api/voices > 10_web/forum_voices.json
curl -s http://127.0.0.1:3002/api/kk > 10_web/forum_kk.json

# Concurrent test
for i in 1 2 3 4; do
  for j in $(seq 1 25); do
    curl -s -X POST http://127.0.0.1:3002/api/forum \
        -H "Content-Type: application/json" \
        -d "{\"voice\":\"leo\",\"question\":\"q${i}_${j}\",\"max_tokens\":15}" \
        > "10_web/concurrent_w${i}_q${j}.json" &
  done
  wait
done
# 100 total requests across 4 workers
```

Per Опус-2 caveat #4 (`aml/README.md:215-221`): forum is sync-accept; concurrent requests will serialize. Plan choice: **document the serialization (record per-request latency) rather than fix during the run**. Total wall time will be ~100 × per-request latency. With 15 tokens at A100 speed ~150 tok/s estimate, per-request ≈ 100 ms → total ≈ 10 s. If observed > 60 s, that's confirmation of full serialization.

Save per-request `started_at` / `finished_at` timestamps to `10_web/concurrent_timings.tsv` and assert mean inter-completion time > 100 ms (proves serialization).

```bash
kill $PID
```

### Pass criteria

- `GET /` returns dario.html bytes (compare with `dario.html` in repo).
- `POST /api/chat` JSON has all expected keys.
- `GET /api/kernel` JSON validates.
- AML forum responds on all 3 endpoints.
- 100/100 concurrent requests succeed (some serialized).

### Failure recovery

If port 3001 or 3002 conflicts with Runpod's reverse proxy, switch to 18801 / 18802 (per `aml/README.md:163`).

**Estimated wall time:** 22 min. **Cost:** ~$0.64.

**Codex audit checkpoint #2:** feed `10_web/concurrent_timings.tsv`, JSON schema validation outputs, and any 4xx/5xx response logs.

---

## 12. Phase 11 — AML / Go / C parity

### Goal
Three identical prompts × three voices × three implementations (AML / Go / C) — capture token-level diffs. Acceptance: ≥ 95% token-position parity for the first 50 tokens given identical seed.

### Inputs
- AML: `./aml/dario_infer`
- Go: `./bin/dario-infer`
- C: `./infer_v4` (direct)

All three should ultimately call `infer_v4` under the hood (AML and Go spawn it as a subprocess per `aml/README.md:47`, `cmd/internal/dario/infer.go:65`). Drift can come from prompt-wrapping differences (Q:/A: vs raw), tokenizer state, or different default flags.

### Voices: leo, yent, resonance-yent (covers both backends).

### Three prompts (reused from Phase 7 to amortize)
1. Technical
2. Philosophical
3. Personal

### Steps

For each (voice, prompt) pair:

```bash
# Identical seed, same temp / top_k / rep_pen for fair comparison
SEED=42; TEMP=0.7; TOPK=40

./aml/dario_infer --voice $VOICE --max-tokens 50 --temp $TEMP --topk $TOPK \
    "$PROMPT" > 11_parity/aml_${VOICE}_${PID}.txt
./bin/dario-infer --voice $VOICE --max-tokens 50 --temp $TEMP --top-k $TOPK \
    --seed $SEED "$PROMPT" > 11_parity/go_${VOICE}_${PID}.txt
./infer_v4 weights/$WEIGHTS_FILE "Q: $PROMPT\nA:" 50 $TEMP $SEED $TOPK \
    > 11_parity/c_${VOICE}_${PID}.txt
```

Tokenize each output the same way (using `infer_v4`'s own BPE — write a tiny C shim that calls `nt_bpe_encode` on each output and dumps token IDs).

Compare token-by-token across the three implementations. Compute pairwise:
- AML vs Go
- AML vs C
- Go vs C

Define **token-position parity** as: fraction of positions in [0, 50) where both impls produce the same token ID.

### Pass criteria

- Mean pairwise parity ≥ 95% across (voice, prompt) cells.
- Per-cell parity ≥ 80% as a hard floor.
- Where parity < 95%, root-cause must be identified — likely Опус-2 caveat #3 (`aml/README.md:206-213`): chat-token compromise. AML & Go wrap as `Q: ...\nA:`; raw C invocation differs. Document the token-position of first divergence.

### Failure recovery

If parity drops below 80%, dump tokenizer outputs side-by-side. Likely one of:
- prompt-wrapping mismatch (Q:/A: extra newline)
- different default rep_pen
- seed propagation difference
Document and proceed; this is a paper-finding, not a bug-fix run.

**Estimated wall time:** 25 min. **Cost:** ~$0.72.

---

## 13. Phase 12 — High-leverage Опус-2 unblockers (OPTIONAL — only if Phases 0–11 all green)

### Decision rule
This phase runs ONLY if every preceding phase landed PASS. If any phase has a hard failure, skip Phase 12 and reserve the budget for re-runs.

### 12.1 Chat-token fix in `infer_v4.c`

Per Опус-2 (`aml/README.md:206-213`): SFT voices were trained on Q/A wrapping with chat tokens BOS=32759 / USER_START=32760 / USER_END=32761 / ASST_START=32762 (and ASST_END=32763 per `README.md:1260`). The current binary takes a raw prompt and BPE-encodes it; chat tokens are not injected. This is "the single highest-leverage fix for Zenodo paper voice quality" per the spec.

Architect spec:
- Add `--chat-tokens` boolean flag to `infer_v4` (`infer_v4.c:493`).
- When set, after BPE-encode (`infer_v4.c:597`), prepend `[BOS, USER_START]` and append `[USER_END, ASST_START]` (special tokens are integer IDs, not BPE-encoded text).
- Generation continues until `ASST_END` (32763) is sampled OR max_tokens reached.

Rebuild: `make infer_v4` (per `Makefile:53-55`).

Re-run a tiny voice quality check: feed leo / arianna / yent the question `"What is resonance?"` with `--chat-tokens`; compare to non-chat-token output. Architect reads both; chat-token output should be more on-voice.

Estimate: 30 min code + 10 min test = 40 min total. **Cost:** ~$1.16.

### 12.2 Duet/trialogue port to AML — DEFERRED

Опус-2 estimated half-day (`aml/README.md:191-192`). Explicit instruction: **do NOT run on RunPod**; flag as follow-up for a Mac Neo session.

### 12.3 Threaded forum — DEFERRED

Опус-2 noted thread-safety needs proper testing in the SQLite layer (`aml/README.md:218-221`). Same: flag as follow-up.

### Pass criteria (12.1 only if attempted)

- `make infer_v4` rebuilds clean with new flag.
- Existing `--chat-tokens=false` (default) path unchanged: byte-for-byte equal output for the canary prompt at seed=42 (regression-test against Phase 11 outputs).
- `--chat-tokens=true` path produces output that ASST_END-terminates within max_tokens for ≥ 50% of voice/prompt cells.
- Architect-judged voice quality improvement on ≥ 1 of 3 voices.

### Failure recovery

If the chat-token fix produces gibberish (special token IDs are wrong, or interleaving is buggy), revert and document.

**Estimated wall time (if run):** 50 min. **Cost (if run):** ~$1.45.

---

## 14. Phase 13 — Documentation pipeline (Zenodo paper appendix)

### Goal
Every phase writes structured artifacts; final aggregation produces the paper appendix.

### Per-phase artifact tree

```
~/arianna/dario/runpod/2026-05-XX/
├── 00_pre/
│   ├── command.sh
│   ├── stdout.log
│   ├── stderr.log
│   ├── metrics.json
│   ├── git_head.txt
│   ├── git_clean.txt
│   ├── build_dario.log
│   ├── build_sartre.log
│   ├── build_kk.log
│   ├── build_full.log
│   ├── build_all.log
│   ├── build_dario_kk.log
│   ├── make_test.log
│   ├── weights_sha256.txt
│   └── smoke.log
├── 01_equation/
│   ├── command.sh stdout.log stderr.log metrics.json
│   └── per_term/{B,H,F,A,V,S,T}.txt
├── 02_chambers/
│   ├── command.sh ... metrics.json
│   ├── kuramoto.tsv
│   └── per_chamber/{FEAR,LOVE,RAGE,VOID,FLOW,COMPLEX}.txt
├── 03_velocity/
│   ├── command.sh ... metrics.json
│   └── priority_test.log
├── 04_seasons/
│   ├── command.sh ... metrics.json
│   └── timeseries.tsv
├── 05_sartre/
│   ├── command.sh ... metrics.json
│   ├── standalone.log
│   ├── kernel.json
│   └── register_models.log
├── 06_kk/
│   ├── command.sh ... metrics.json
│   ├── q1.txt
│   ├── multi_essay_*.txt
│   └── lineage.tsv
├── 07_voices/
│   ├── command.sh ... metrics.json
│   ├── transcripts/<180 files>
│   ├── scores.tsv
│   └── score.sh
├── 08_modes/
│   ├── command.sh ... metrics.json
│   └── transcripts/{chain,dialogue,explore,duet,trialogue}.txt
├── 09_cross_arch/
│   ├── command.sh ... metrics.json
│   └── transcript.txt
├── 10_web/
│   ├── command.sh ... metrics.json
│   ├── chat_resp.json
│   ├── kernel.json
│   ├── forum_*.json
│   └── concurrent_timings.tsv
├── 11_parity/
│   ├── command.sh ... metrics.json
│   └── {aml,go,c}_<voice>_<pid>.txt
├── 12_unblockers/  (optional)
│   └── chat_tokens_*.txt
└── paper_appendix_dario_runpod_2026_05_XX.md
```

### `metrics.json` schema (per phase)

```json
{
  "phase": "07_voices",
  "started_at": "2026-05-XX T HH:MM:SS Z",
  "finished_at": "2026-05-XX T HH:MM:SS Z",
  "exit_code": 0,
  "wall_time_seconds": 1320,
  "estimated_cost_usd": 0.87,
  "key_findings": [
    "leo optimal: temp=0.5 top_k=40 rp=1.4 (deviates from default 0.75 / 40 / 1.4)",
    "yent optimal: temp=0.8 top_k=0 rp=1.35 (deviates: top_k=∞ revealed coherent voice)",
    "..."
  ],
  "pass": true,
  "notes": "Per-voice scores in scores.tsv; 178/180 cells produced output, 2 timeouts re-ran with seed=43."
}
```

### `make_zenodo_appendix.sh`

Bash + jq aggregator:

```bash
#!/usr/bin/env bash
set -e
ROOT="${1:-./runpod/2026-05-XX}"
APPENDIX="$ROOT/paper_appendix_dario_runpod_2026_05_XX.md"

cat > "$APPENDIX" <<HDR
# Dario RunPod Stress Test — Appendix
Date: $(date -u +%F)
Pod: A100 80GB SXM
Repo HEAD: $(cat "$ROOT/00_pre/git_head.txt")
Total cost (cumulative): $TOTAL_USD
HDR

for d in "$ROOT"/[0-9]*; do
  phase=$(basename "$d")
  jq -r --arg p "$phase" '
    "## " + $p,
    "Started: " + .started_at,
    "Finished: " + .finished_at,
    "Wall time: " + (.wall_time_seconds|tostring) + "s",
    "Pass: " + (.pass|tostring),
    "Findings:",
    (.key_findings[] | "- " + .),
    ""
  ' "$d/metrics.json" >> "$APPENDIX"
done
```

The appendix per `memory/todo_paper_dario.md` is paranoid-mode — every claim cited, no fabricated numbers.

### Pass criteria

- Every phase folder has `command.sh`, `stdout.log`, `stderr.log`, `metrics.json`.
- `paper_appendix_*.md` aggregates 13 phase blocks.
- Total cost figure sums to within 5% of the per-phase estimates.

**Estimated wall time:** 8 min (final aggregation only). **Cost:** ~$0.23.

**Codex audit checkpoint #3:** feed `paper_appendix_*.md` + the per-phase `metrics.json` files. Architect handles edits.

---

## 15. Codex audit checkpoints — schedule

| Checkpoint | After phase | Inputs to feed | Decision criteria |
|---|---|---|---|
| #0 | Phase 0 | `00_pre/build_*.log`, `make_test.log`, `git_head.txt`, `weights_sha256.txt` | All builds clean; tests pass; weights match HF SHAs. |
| #1 | Phase 4 | `04_seasons/timeseries.tsv`, plus 01-03 metrics | Equation correctness verified; all 7 forces dominate; chambers + velocities work. |
| #2 | Phase 7 | `07_voices/scores.tsv`, top-3 transcripts/voice, methodology Bash | Per-voice optimum identified; lock-in approval. |
| #3 | Phase 10 | `10_web/concurrent_timings.tsv`, JSON validation outputs | Web UI green; concurrent serialization documented. |
| #4 | Phase 13 | `paper_appendix_*.md`, all `metrics.json` | Final paper appendix coherent; no fabricated numbers; cost reconciled. |

Architect-only between checkpoints. Singularity mode: failure → diagnose → minimal fix → rerun, no per-step approval. Three-strikes rule from CLAUDE.md applies: on third unproductive retry, stop and report root cause to architect.

---

## 16. Cost discipline

| Phase | Estimated wall time | Estimated cost (A100 80GB @ $1.74/hr) |
|---|---|---|
| 0 — Pre-flight | 12 min | $0.35 |
| 1 — Equation | 25 min | $0.72 |
| 2 — Chambers | 35 min | $1.02 |
| 3 — Velocity | 18 min | $0.52 |
| 4 — Seasons + laws | 25 min | $0.72 |
| 5 — SARTRE | 35 min | $1.02 |
| 6 — KK | 40 min | $1.16 |
| 7 — Voice sweep | 30 min | $0.87 |
| 8 — Modes | 35 min | $1.02 |
| 9 — Cross-arch duet | 12 min | $0.35 |
| 10 — Web UI | 22 min | $0.64 |
| 11 — Parity | 25 min | $0.72 |
| 12 — Unblockers (opt.) | 50 min | $1.45 |
| 13 — Documentation | 8 min | $0.23 |
| **Total (no Phase 12)** | **322 min ≈ 5.4h** | **$9.34** |
| **Total (with Phase 12)** | **372 min ≈ 6.2h** | **$10.79** |

Buffer: 30% overhead for build re-runs / failed cells / debug. 6.2h × 1.3 = **8.1h ≈ $14.10**. Within the $14 target.

Hard stop: 12h elapsed. Wall-clock check via `date` at every phase start; if `(now - boot_time) > 12h`, save state and shut down.

Per-phase cost logged to `metrics.json` as `wall_time_seconds × 1.74 / 3600`.

---

## 17. Open risks / known gotchas

| # | Risk | Source | Mitigation |
|---|---|---|---|
| 1 | Опус-2 caveat #3: chat-token compromise (Q:/A: wrap, BOS not injected) | `aml/README.md:206-213`, `cmd/internal/dario/infer.go:60-70` | Phase 11 parity will likely surface this; Phase 12 fixes it if budget permits. |
| 2 | Forum sync-accept blocks under load | `aml/README.md:215-221` | Phase 10 documents serialization; do not patch under fire. |
| 3 | 8-emotion fingerprint zeros in C | `aml/README.md:192-201` | Phase 6 Charged KK records observed=zeros; flag for paper. |
| 4 | `make test` 1725/1725 claim unverified | `README.md:512` (claim), `tests/test_dario.c:23-24` (dynamic counter) | Phase 0 captures actual count; report match/mismatch. |
| 5 | KK SQLite db path conflicts with concurrent voices | inferred from sqlite single-writer | Per-process DB path in `KK_DB_PATH` env var; confirmed setter at `dario.c:1806`. |
| 6 | Build #6 (dario+kk no sartre) may not exist as a stock target | `Makefile` doesn't include this combination | Manual cc invocation; if it fails to compile, README.md:525 ("every file compiles alone") is partially-aspirational; document. |
| 7 | rep_penalty hardcoded in infer_v4 | `infer_v4.c:627` | Phase 7 builds three rp variants instead of patching the canonical. |
| 8 | Dario.html / forum.html may not exist on the pod (they're served from disk per `aml/README.md:114`) | filesystem | `00_pre` confirms both files via `ls`. |
| 9 | RunPod's reverse proxy may block ports 3001/3002 | Runpod docs | Fall back to 18801/18802 (Опус-2's known-working ports per `aml/README.md:163`). |
| 10 | `dario_memory.db-shm` / `-wal` left over in repo | `ls /Users/ataeff/arianna/dario/` shows them 2026-05-08 | Phase 0 wipes any pre-existing `.db*` files at the canonical path before running KK tests. |
| 11 | A100 80GB SXM may be 40GB at provisioning if Runpod default differs | provisioner | Phase 0 step 1.1 verifies via `nvidia-smi`. |
| 12 | `make weights` requires `hf` CLI authenticated | Makefile:73 | Phase 0 toolchain check confirms `hf whoami` works; auth via `HF_TOKEN` env. |
| 13 | Sweep grid 180 cells × ~2s ≈ unfeasibly long if A100 isn't 5-10× faster than Neo | speed assumption | First Phase 7 cell measures actual tok/s; abort sweep if < 50 tok/s and reduce cells to 60 (drop rep_pen axis). |
| 14 | Phase 5 flag-setter API may not exist | `README.md:639` "currently set externally" | Phase 5 step 6.9 explicitly documents this; no API → finding for paper. |
| 15 | Phase 8 README sample reproduction — exact text WILL NOT match (different state) | sampling stochasticity | Acceptance lowered to 30% lexical overlap on 1 of 6 turns; voice register the real goal. |

---

## 18. Memory rules in effect during execution

These bind the architect during the run:

1. **Provenance gate.** Every number / file / line / hash inline-cited as `X = Y (file.c:NNN | git log abcdef | hf show)`. No bare claims (per `protocol_claim_verification_gate.md` and CLAUDE.md gate #2).
2. **Ban on the diagonal-baseline optimizer name.** The string "Adam" never appears in any output, log, transcript, commit message, or paper appendix (per `feedback_adam_ban_2026_04_29.md`). Use "Chuck" or "diagonal optimizer baseline" if such a discussion arises.
3. **Python ban on inference path.** No Python in `dario` / `infer_v4` / `kk_kernel` / `sartre_kernel` runtime invocations. Bash + AWK + jq + sqlite3 + the C/AML/Go binaries. Python is allowed for: `hf` CLI (auth + download), Phase 13 aggregation IF chosen over Bash (the `make_zenodo_appendix.sh` here is intentionally Bash to honor the rule), and any data-prep one-offs flagged ahead of time (per `feedback_python_ban_2026_04_29.md`).
4. **No closed-milestone retraining.** Read-only weights from `ataeff/dario` HF repo. No fine-tuning, no LoRA training, no checkpoint modification, no `make weights` re-upload (per `feedback_failure_unsolicited_finetune_2026_04_27.md`).
5. **Logs / metrics from real tool output only.** If `make test` prints "1684 / 1684 passed", record exactly that; do not paste "1725" because the README says so. The README is a claim; the log is the evidence.
6. **AI is not a tool.** In any prose written into transcripts or the paper appendix, AI is referred to as "organism", "field", "function", "component" — never "tool" (per CLAUDE.md "Specific bans").
7. **Train loss reporting standard.** Phase 7 voice quality reports use train-loss-first if any retraining has happened (it has not in this plan). Inference-only runs report tokens/s + temperature + top_k + rep_pen + seed (per `feedback_show_train_loss.md`).
8. **No self-removal / no helpful-assistant register.** The transcripts are technical, not chatty. (per `feedback_no_helpful_assistant.md`).
9. **Default = unverified.** If the architect or reviewer asks "did the model output X tokens?" — reply "не проверил, сейчас сверю" if not in `metrics.json`, then read the file (per CLAUDE.md gate #2).
10. **Singularity loop discipline.** On failed phase: reproduce → 1 hypothesis → minimal change → re-run. Three strikes without new knowledge → stop, report. (per CLAUDE.md Workflow #5).

---

End of plan v1.
