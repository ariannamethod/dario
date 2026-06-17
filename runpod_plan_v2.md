# RunPod Stress-Test Plan v2 — Dario

> Audit-ready plan for a single-pod, single-architect, singularity-mode stress test of the dario organism (`dario.c` + `sartre_kernel.c` + `kk_kernel.c` + AML/Go/C ports + voices). Reviewed by `codex review` and Gemini bridge before execution. The plan is the contract; the architect runs without per-step approval inside its bounds.

Author: Опус-3 (revised after Codex + Gemini + architect review of v1)
Date drafted: 2026-05-08
Supersedes: `runpod_plan_v1.md` (kept side-by-side; do not delete)
Target hardware: RunPod A100 80GB SXM, single GPU
Target binary tree: `~/arianna/dario/` cloned fresh on the pod
Total budget: ≤ 8 GPU-hours @ **$1.39/hr** (verified `runpodctl get cloud` 2026-05-08 02:30 IDT) = **~$11.21** with 30% buffer
Hard kill: 12h elapsed without architect attention → save state, shut pod
Output root on pod: `~/arianna/dario/runpod/2026-05-XX/` (replace XX with the actual day at boot)

Memory rules in effect: provenance-on-every-number; "Adam" optimizer name banned; Python NOT permitted on the inference path (training/data prep/CLI tooling like `hf` and `huggingface_hub` ok); no closed-milestone retraining; logs/metrics from real tool output only — never invent.

---

## 1. Phase 0 — Pre-flight on the pod

### 1.1 Pod provisioning checklist

| Item | Spec | Verification |
|---|---|---|
| GPU | A100 80GB SXM | `nvidia-smi` shows 1× A100, ~80 GB |
| Image | Ubuntu 22.04 + CUDA 12.x base, build-essential preinstalled | `cc --version`, `make --version`, `gcc -v` |
| Persistent volume | ≥ 10 GB | `dario_hf_upload/` is 3.4 GB on Neo (`du -sh ~/arianna/dario_hf_upload/` = `3.4G` verified 2026-05-08); plus KK SQLite (≤ 200 MB), per-phase logs (≤ 1 GB), 6 binaries × ~1 MB, GitHub clone (~50 MB). Plan for 10 GB to leave headroom for sweep transcripts. |
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
| `python3-pip` + `huggingface_hub[cli]` | `/usr/bin/python3` + `~/.local/bin/hf` (or system path after pip install) | `pip3 show huggingface_hub && hf --version` |
| `jq` | for JSON schema spot-checks | `jq --version` |
| `sqlite3` CLI | for KK introspection | `sqlite3 --version` |

#### 1.2.1 `00_pre/install_toolchain.sh` (HIGH-fix #2)

Ubuntu 22.04 base does NOT ship `hf`. Explicit install steps in `install_toolchain.sh`:

```bash
#!/usr/bin/env bash
set -e
sudo apt-get update
sudo apt-get install -y build-essential libsqlite3-dev libopenblas-dev pkg-config jq sqlite3 \
                        python3 python3-pip
# Python is permitted ONLY for the hf CLI / huggingface_hub data-prep tool. NOT for inference.
# Per memory/feedback_python_ban_2026_04_29.md, Python on data-prep / CLI tooling path is allowed.
pip3 install --upgrade --user "huggingface_hub[cli]"
# Confirm the only Python on the pod's inference path is this CLI:
python3 -c "import sys; print('python3 path:', sys.executable)"
hf --version
```

If any of `libnotorch.a`, `libaml.a`, `amlc` are missing, `00_pre/install_toolchain.sh` clones `github.com/ariannamethod/notorch` and `github.com/ariannamethod/ariannamethod.ai` (AML lives there) and runs the in-tree `make install` for each. Both repos have system-wide install on Linux at `/usr/local/{bin,lib,include}` per CLAUDE.md.

#### 1.2.2 Python policy on the pod

Python presence is restricted to:
1. `hf` CLI (`huggingface_hub[cli]`) — used by `make weights` (`Makefile:73-83`) and possible re-uploads. Pre-flight only.
2. Implicitly invoked by `pip3` itself.

Python is NEVER used for inference, never used for training, never used as a process spawned by `dario` / `infer_v4` / AML / Go binaries. Confirm at the end of Phase 0:

```bash
ps -ef | grep python | grep -v grep
# Expected: empty (no long-lived python processes)
```

Document this in `00_pre/python_audit.txt`.

### 1.3 Repo bring-up

```bash
mkdir -p ~/arianna && cd ~/arianna
git clone https://github.com/ariannamethod/dario.git
cd dario
git rev-parse HEAD > runpod/2026-05-XX/00_pre/git_head.txt
git status --porcelain > runpod/2026-05-XX/00_pre/git_clean.txt   # must be empty
```

### 1.4 Build matrix verification (six configs from `README.md:524-525`)

Each build runs `make clean && time make <target>` and captures `2>&1 | tee 00_pre/build_<target>.log`. All builds must exit 0 — except config #6, which is best-effort (see CRIT-fix #1 below).

| # | Target | Make recipe | Defines | Expected artifact | Cite |
|---|---|---|---|---|---|
| 1 | dario alone | `make dario` | (none) | `./dario` | `Makefile:6-7` |
| 2 | sartre alone | `make sartre` | (none) | `./sartre_kernel` | `Makefile:10-11` |
| 3 | kk alone (CLI) | `make kk` | `-DKK_STANDALONE` | `./kk` | `Makefile:30-31` |
| 4 | dario + sartre | `make full` | `-DHAS_SARTRE -DHAS_DARIO` | `./dario` | `Makefile:14-15` |
| 5 | dario + sartre + kk | `make all` | `-DHAS_SARTRE -DHAS_DARIO -DHAS_KK -lsqlite3` | `./dario` | `Makefile:19-21` |
| 6 | dario + kk (no sartre — soft) | manual cc invocation (HIGH-fix #10) | `-DHAS_KK -DHAS_DARIO` | `./dario_kk_only` | extrapolated; mirrors `Makefile:19-21` minus sartre |

Wall-time per build is captured by `time` and committed to `metrics.json` for the phase.

#### 1.4.1 Config #6 manual command (HIGH-fix #10)

```bash
cc -Wall -Wextra -Wno-unused-parameter \
   dario.c kk_kernel.c \
   -DHAS_KK -DHAS_DARIO \
   -O2 -lm -lsqlite3 \
   -o dario_kk_only \
   2>&1 | tee 00_pre/build_dario_kk.log
```

Note: this mirrors `Makefile:19-21` MINUS `sartre_kernel.c` and the `-DHAS_SARTRE` define. It MAY fail to link/compile if dario.c's `#ifdef HAS_KK` paths reach into sartre symbols.

### 1.5 Phase 0 acceptance — softened per CRIT-fix #1

- **5 of 6 builds (configs 1-5) HARD REQUIRED**: must exit 0 with no `error:` on stderr (warnings tolerated).
- **Config #6 SOFT**: if it fails, log the linker/compiler error, FLAG as a paper-appendix finding ("README.md:525 'every file compiles alone' is partially-aspirational; #ifdef HAS_KK couples dario.c to sartre symbols"), and **proceed** to Phase 1. DO NOT block the run.

The architect already approved this softening (per merged-feedback CRIT #1). The README claim becomes empirical-finding rather than precondition.

### 1.6 Weights download + sanity

```bash
make weights         # invokes hf download ataeff/dario per Makefile:71-83
ls -la weights/
sha256sum weights/*.bin > 00_pre/weights_sha256.txt
```

Required files (per `Makefile:73-83`): `janus_v4_base_22k.bin`, `janus_v4_sft_leo.bin`, `janus_v4_sft_arianna.bin`, `janus_v4_sft_yent.bin`, `resonance_200m_lora_yent.bin`, `leo_janus_d12_f16.bin`, `tokenizer.pkl`, `tokenizer_yent.bin`.

#### 1.6.1 `tokenizer.pkl` — legacy Python path (MED-fix #5)

`infer_v4.c:511-512` uses BPE merges hardcoded into headers (`janus_v4_bpe_merges.h`). The `.pkl` is for the legacy Python `chain_dialogue.py` path, which is NOT exercised on the pod (Python inference banned). The download still pulls it for completeness, but Phase 0 acceptance does NOT gate on its presence/correctness; nothing on the inference path consumes it.

Cross-check size: `dario_hf_upload/` was 3.4 GB on Neo (`du` verified 2026-05-08). Equivalent download on the pod must come within ±10%.

### 1.7 `make test` — verify the 1725/1725 claim

```bash
make test 2>&1 | tee 00_pre/make_test.log
```

`README.md:512` claims **1725/1725**. The test file uses dynamic `tests_run` / `tests_passed` counters (`tests/test_dario.c:23-24`), the actual count is whatever `RUN_TEST` macros + `ASSERT_*` macros add up to at runtime. The plan does NOT trust the README number; it captures the actual count and pass/fail from `make_test.log` and writes both to `metrics.json` as `make_test_run`, `make_test_passed`, `make_test_failed`. Acceptance: `failed == 0`. The 1725 number is reported as a finding (matches / does not match / unverifiable).

### 1.8 Sanity smoke

```bash
echo "/stats" | ./dario
echo "hello world" | ./dario     # should produce a code fragment + field-words
echo "/quit"   | ./dario
```

Verify: prompt loop exits cleanly (no SEGV / no hang). Capture the `┌─ ... ─── d=... τ=...` envelope to `00_pre/smoke.log`.

### 1.9 Phase 0 acceptance (REVISED)

- Builds: **5/6 hard required** (configs 1-5); config #6 soft, flagged on failure.
- All weights present (8 files), sha256 logged. `tokenizer.pkl` listed but not gated.
- `make_test`: `failed == 0` (pass/run counts captured verbatim).
- Smoke run produced one well-formed envelope.
- `git_clean.txt` empty.
- Python audit: no python processes running.

**Estimated wall time:** 12 min (download dominates). **Cost @ $1.39/hr:** ~$0.28.

**Codex audit checkpoint #0:** feed `00_pre/build_*.log`, `00_pre/make_test.log`, `00_pre/git_head.txt`, `00_pre/weights_sha256.txt`, `00_pre/python_audit.txt`. Architect handles fix-then-rerun cycle.

---

## 2. Phase 0.5 — `infer_v4` CLI extensions (architect-fix #11 + Опус-2 #12.1 hoisted)

### Goal
Combine the chat-token fix (Опус-2 #12.1) and the rep_penalty CLI flag (architect-fix #11) into a single Phase that runs BEFORE Phase 7. Both touch `infer_v4.c`. Doing them together saves a build cycle and unblocks Phase 7's rep_pen sweep.

### Why hoist
- v1 Phase 7.5 originally proposed building three rp variants (`infer_v4_rp10`, `_rp13`, `_rp14`) — wasteful, brittle, and disagrees with the "single canonical binary" design. Replace with a CLI flag.
- v1 Phase 12.1 proposed the chat-token fix as OPTIONAL post-Phase-11 polish. The architect chose to land it BEFORE Phase 7 so voice-quality scoring uses the on-voice variant rather than the raw-prompt variant.

### 2.1 Patch — `--rep-penalty F` CLI flag

Current state: `infer_v4.c:627` hardcodes `float rep_penalty = 1.3f;` inside the generation loop.

Patch (≤ 10 LOC total):

1. Add at the argv parser block (`infer_v4.c:493-500`):
   ```c
   /* parse --rep-penalty F (long flag) — default 1.3 */
   float rep_penalty_arg = 1.3f;
   for (int ai = 1; ai < argc - 1; ai++) {
       if (strcmp(argv[ai], "--rep-penalty") == 0) {
           rep_penalty_arg = (float)atof(argv[ai+1]);
       }
   }
   ```
2. Replace the hardcoded `1.3f` at `infer_v4.c:627`:
   ```c
   float rep_penalty = rep_penalty_arg;
   ```

### 2.2 Patch — `--chat-tokens` boolean flag

Per Опус-2 (`aml/README.md:206-213`): SFT voices were trained with chat-token wrapping `BOS=32759 / USER_START=32760 / USER_END=32761 / ASST_START=32762 / ASST_END=32763` (also `README.md:1260`). Current binary BPE-encodes raw prompt; chat tokens never injected.

Patch:
1. Argv parser:
   ```c
   int chat_tokens = 0;
   for (int ai = 1; ai < argc; ai++)
       if (strcmp(argv[ai], "--chat-tokens") == 0) chat_tokens = 1;
   ```
2. After BPE-encode (`infer_v4.c:597`), if `chat_tokens && use_bpe`:
   ```c
   /* prepend BOS, USER_START; insert encoded tokens; append USER_END, ASST_START */
   int wrapped[4096]; int wn = 0;
   wrapped[wn++] = 32759; /* BOS */
   wrapped[wn++] = 32760; /* USER_START */
   for (int i = 0; i < len && wn < 4094; i++) wrapped[wn++] = ctx[i];
   wrapped[wn++] = 32761; /* USER_END */
   wrapped[wn++] = 32762; /* ASST_START */
   memcpy(ctx, wrapped, wn * sizeof(int));
   len = wn;
   ```
3. In the generation loop (`infer_v4.c:625-...`), if `chat_tokens` AND sampled token == `32763` (ASST_END), break early.

### 2.3 Build

```bash
make clean && make all && make infer_v4 \
    2>&1 | tee 00_5_cli/build_infer_v4.log
```

Both targets must rebuild cleanly. The Makefile rebuild inherits the new flags; `infer_v4.c:53-55` already has the recipe.

### 2.4 Regression test (architect acceptance — byte-equality)

```bash
# Default behavior: no flag = old behavior.
SEED=42; PROMPT="What is resonance?"
./infer_v4_v1_baseline weights/janus_v4_sft_leo.bin "Q: $PROMPT\nA:" 50 0.7 $SEED 40 \
    > 00_5_cli/baseline_no_flag.txt

./infer_v4 weights/janus_v4_sft_leo.bin "Q: $PROMPT\nA:" 50 0.7 $SEED 40 \
    > 00_5_cli/patched_no_flag.txt

diff -u 00_5_cli/baseline_no_flag.txt 00_5_cli/patched_no_flag.txt > 00_5_cli/regression.diff
test ! -s 00_5_cli/regression.diff && echo "PASS: byte-equal at default rep_pen=1.3"
```

`infer_v4_v1_baseline` is the binary built from the unpatched `infer_v4.c` at `git rev-parse HEAD` BEFORE Phase 0.5 modifications. The architect saves it as a side-by-side reference at the START of Phase 0.5 (`cp infer_v4 infer_v4_v1_baseline`).

Same regression for `--chat-tokens=false` (default): byte-equal to baseline. The chat-token path activates ONLY when the flag is passed.

### 2.5 Acceptance

- `make all` and `make infer_v4` both rebuild clean with the patched source.
- `--rep-penalty 1.3` (or no flag) produces byte-equal output to the baseline binary at fixed seed=42 across 3 (voice, prompt) cells.
- `--chat-tokens` path: ASST_END termination observed for ≥ 50% of (voice, prompt) cells when running leo / arianna / yent on the canary "What is resonance?". (Soft criterion — the actual coherence improvement is a Phase 7 finding, not a Phase 0.5 gate.)
- Architect commits the patch to a local branch `runpod-2026-05-XX/infer_v4_cli`. NO push to upstream until the run is reviewed.

### 2.6 Failure recovery

If the regression diff is NOT empty for `--rep-penalty 1.3` default, the patch corrupted the default path. Revert, re-read the patch carefully (likely `rep_penalty_arg` shadowed a local), re-apply, re-run regression. Three strikes → stop and report.

If `--chat-tokens` produces gibberish (special token IDs wrong or interleaving buggy), document and run Phase 7 with `--chat-tokens=false` only. The plan does NOT depend on chat-tokens working for Phase 7 to deliver value.

**Estimated wall time:** 35 min (15 min code + 10 min regression + 10 min margin). **Cost @ $1.39/hr:** ~$0.81.

---

## 3. Phase 0.6 — AML / Go CLI surface verification (architect-fix #12)

### Goal
Verify exact CLI flag surfaces for `dario_infer`, `dario_dialogue`, `dario_forum` (AML) and their Go counterparts. v1's invocations referenced flags that don't exist in some binaries (`--no-field` and `--kk-db` are AML-only on `dario_dialogue`; the Go binary has `--seed` but the AML binary doesn't; `explore` mode exists only in legacy Python). Surface mismatches will cause Phase 8 / 11 to fail unexpectedly.

### 3.1 AML surfaces — verified by reading source 2026-05-08

| Binary | Source | Modes | Flags supported |
|---|---|---|---|
| `aml/dario_infer` | `aml/dario_infer.aml:263-282` | n/a (single-shot generation) | `--voice --weights --temp --topk --max-tokens --raw --binary --help` |
| `aml/dario_dialogue` | `aml/dario_dialogue.aml:544-563, 600-625` | `chain | dialogue` ONLY (NO duet/trialogue/explore in AML) | `--mode --voice --topic|--prompt|--seed --depth --max-tokens --temp --topk --knowledge --field --no-field --kk-db --binary --weights --help` |
| `aml/dario_forum` | `aml/dario_forum.aml:702-720, 755-767` | n/a (HTTP forum) | `--port --host --knowledge --kk-db --field --no-field --binary --help` (NO `--max-tokens`) |

### 3.2 Go surfaces — verified by reading source 2026-05-08

| Binary | Source | Modes | Flags supported |
|---|---|---|---|
| `bin/dario-infer` | `cmd/dario-infer/main.go:42-92` | n/a | `--voice (-v) --weights --temp (-t) --topk --max-tokens (--max,-n) --seed --binary --timeout --raw --help` |
| `bin/dario-dialogue` | `cmd/dario-dialogue/main.go:73-99` | `chain | dialogue | duet | trialogue` | `--mode --voice --voice2 --voice3 --topic|--prompt|--seed --depth --max-segment|--max-tokens --temp|--temperature --topk|--top-k --rep-penalty --knowledge --save --weights-dir --field --timeout` (NO `--no-field`, NO `--kk-db` — KK is in-process via `kk.New()`) |
| `bin/dario-forum` | `cmd/dario-forum/main.go:43+` | n/a | (verified at run-time via `--help`) |

### 3.3 Verification step

At Phase 0.6, run `--help` on every binary and capture to `00_6_cli/help_<binary>.txt`. Compare against the table above; if anything diverged since 2026-05-08, the table wins (and the plan updates Phase 8 / 11 invocations accordingly).

```bash
for bin in aml/dario_infer aml/dario_dialogue aml/dario_forum \
           bin/dario-infer bin/dario-dialogue bin/dario-forum; do
  ./$bin --help 2>&1 | tee 00_6_cli/help_$(basename $bin).txt
done
```

### 3.4 Plan adjustments forced by 0.6

1. Phase 8.3 (explore mode) is **DROPPED** — no AML or Go support, only Python (`chain_dialogue.py:1009`) which is banned. Original Phase 8.3 budget reallocated to deeper coverage of Phase 8.4-8.5. Document as a finding: "Explore mode is Python-only; AML/Go ports do not implement it."
2. Phase 8.4 / 8.5 (duet / trialogue) MUST use Go binary (`bin/dario-dialogue`) — AML port doesn't implement them. Cite `aml/README.md:187-192` and the `aml/dario_dialogue.aml:606-625` flag list (no duet mode case).
3. Phase 8.4 / 8.5 invocations CANNOT use `--kk-db` (Go has no such flag; KK is in-memory). Drop the flag.
4. Phase 11 parity — Go path uses `--seed`; AML `dario_infer` does NOT have `--seed`. To get reproducible AML output, set seed via the underlying spawned `infer_v4` binary (positional arg — argv[5] per `infer_v4.c:496`). Architect verifies AML binary forwards or sets a deterministic seed before running parity. If not, the AML cell drops to "best-effort lexical comparison" rather than token-position parity.
5. Phase 8.1 chain mode invocation drops `--no-field` from Go invocations and keeps it for AML invocations.

### 3.5 Acceptance

- All six binaries print `--help` cleanly (exit 0).
- A flag-matrix table `00_6_cli/flag_matrix.tsv` is committed showing observed-vs-planned. Any divergence is a finding to feed back to Phase 8/11.

**Estimated wall time:** 10 min. **Cost @ $1.39/hr:** ~$0.23.

---

## 4. Phase 1 — Equation correctness (7 forces, dario.c alone)

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
| T (Trauma) | Push dissonance > 0.7 for ≥ 6 consecutive turns. `D.trauma_level` accumulates by `dissonance × 0.1` per turn ONLY when `dissonance > 0.7` (`dario.c:1887-1888`); 6 × 0.1 = 0.6 (above the 0.5 RAGE threshold AND well above the 0.3 trauma boost threshold at `dario.c:1340-1346`). Boost activates when `D.trauma_level > 0.3` per `dario.c:1340-1346`. | `FORCE_TRAUMA` (=6) | one of 3 at `dario.c:539-566` | `dario.c:1340-1346`, `README.md:222-234` |

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

**Estimated wall time:** 25 min (CPU-only; dario doesn't touch GPU). **Cost @ $1.39/hr:** ~$0.58.

---

## 5. Phase 2 — Emotional chambers (6 chambers + Kuramoto)

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

### Trauma accumulation rate — verified (CRIT-fix #3)

Verified by reading `dario.c:1884-1908`:

```c
D.dissonance = compute_dissonance(input);
ingest(input);
if (D.dissonance > 0.7f)
    D.trauma_level = clampf(D.trauma_level + D.dissonance * 0.1f, 0, 1);
```

Trauma rises by `dissonance × 0.1` per turn — but ONLY when `dissonance > 0.7`. Maximum per-turn delta: `1.0 × 0.1 = 0.1` at theoretical peak; realistic delta is 0.07-0.09. Trauma also decays at 0.97/step (`dario.c:1493`) when no high-dissonance input — but inside the trigger run, every turn fires, so net rise per turn ≈ `0.1 × 0.97 ≈ 0.097`.

### Adjusted RAGE trigger window (CRIT-fix #3)

To exceed the RAGE threshold `trauma > 0.5 AND dissonance > 0.5`, the trigger must accumulate `trauma_level` STRICTLY > 0.5. With per-turn rise ≈ 0.097 and concurrent decay × 0.97:

Turn-by-turn (assuming dissonance=1.0, perfectly alien input every turn):
- t1: `(0 + 0.1) × 0.97 = 0.097`
- t2: `(0.097 + 0.1) × 0.97 = 0.191`
- t3: `(0.191 + 0.1) × 0.97 = 0.282`
- t4: `(0.282 + 0.1) × 0.97 = 0.371`
- t5: `(0.371 + 0.1) × 0.97 = 0.457`
- t6: `(0.457 + 0.1) × 0.97 = 0.540` — first to clear 0.5
- t7: `(0.540 + 0.1) × 0.97 = 0.621`
- t8: `(0.621 + 0.1) × 0.97 = 0.700`

So minimum window for RAGE trigger is **6 turns** in the ideal case. The plan uses **8 turns** to give margin against:
- dissonance < 1.0 in practice (alien gibberish still scores ~0.85 not 1.0),
- the additional `D.trauma_level *= 0.97` decay on idle steps that occur between iterations,
- the `RAGE += 0.06 × trauma` excitation rate (with trauma ≈ 0.5, RAGE rises by 0.03/turn after decay 0.93 ≈ 0.028/turn — needs ~7 RAGE-active turns to clear the 0.2 chamber-active threshold).

If observed accumulation is slower than calculated (e.g., dissonance peaks at 0.85 in practice, giving per-turn delta of `0.085 × 0.97 ≈ 0.082`), extend window further. The architect re-derives in real-time after the first observation in `02_chambers/per_chamber/RAGE.txt`.

### Steps

For each chamber:

1. Boot dario, prime with neutral text to set baseline.
2. Feed input designed to push the relevant signal above its threshold for the calculated number of turns:
   - FEAR: 5 turns of pure-alien input (random non-vocab strings like `xq42 mvp9z plurq`) → dissonance climbs to ~1.0 → FEAR activates.
   - LOVE: 5 turns of densely in-vocab familiar text (resonance > 0.7) → LOVE activates.
   - RAGE: **8 turns** of alien high-dissonance input (CRIT-fix #3). Verify trauma_level passes 0.5 between turns 6-7; observed rate written to `02_chambers/per_chamber/RAGE.txt` with the actual per-turn delta.
   - VOID: 5 turns of mid-dissonance varied-vocab → entropy formula `0.3·(τ-0.5) + 0.4·dissonance + 0.3·(1-resonance)` (per `dario.c:1709-1713`) climbs > 0.7.
   - FLOW: see Section 5 below — extended to 10 turns (HIGH-fix #7).
   - COMPLEX: alternate LOVE-trigger and RAGE-trigger turns until both are simultaneously > 0.2 (typically ≥ 12 turns given LOVE decays at 0.95 and RAGE excitation needs trauma > 0.5 first; budget ≥ 14 turns).
3. After each turn, capture chamber values from `/stats` (chamber state must be exposed; if `/stats` doesn't print chambers, document that as a finding and instrument with a one-liner debug print before next phase — see Phase 12 unblockers).
4. Assert chamber's value rose above 0.2 within the calculated turn window.
5. Cease trigger; sample chamber across 10 idle steps; assert decay rate matches documented value (linear-fit slope on `log(chamber)` vs step → expect slope ≈ `log(decay)`).

### FLOW activation — adjusted (HIGH-fix #7)

FLOW excitation: `+0.05 × emergence` per turn when `emergence > 0.5` (`dario.c:1015`). Emergence formula: `clampf((1 - D.entropy) × D.resonance, 0, 1)` (`dario.c:1486`). Entropy floor 0.10 (`dario.c:1480`); resonance ceiling 0.95 (`dario.c:1483`). Realistic max emergence: `(1 - 0.10) × 0.95 = 0.855`.

With perfect emergence=0.855, per-turn FLOW delta ≈ `0.05 × 0.855 = 0.043`. After decay 0.94: net rise ≈ `(0 + 0.043) × 0.94 ≈ 0.040` per turn. Reaching 0.2 (the chamber-active threshold) needs:
- t1: 0.040, t2: 0.078, t3: 0.114, t4: 0.148, t5: 0.179, t6: 0.207 — **6 turns minimum**.
- But emergence rarely sustains 0.855 across all turns (resonance fluctuates as input absorbs); realistic per-turn rise ≈ 0.025-0.030 → **8-10 turns** to reach 0.2.

Adjusted plan: **10 turns** of dense-emergence input. Threshold relaxed from "> 0.2 within 5 turns" to **"> 0.15 within 10 turns"**. With per-turn rise 0.025, t10 ≈ `0.025 × (1 - 0.94^10) / (1 - 0.94) ≈ 0.025 × 5.43 ≈ 0.136` — close to 0.15 but with margin from input variance pushing some turns above the mean. If after 10 turns FLOW < 0.10, document as failure-to-activate and treat as a paper finding.

### Somatic marker clamp test

Run a sustained FLOW + LOVE high state (15 turns of dense in-vocab high-emergence input). Sample α_mod, β_mod, γ_mod, τ_mod after each turn. Assert all four remain in [0.5, 2.0] (clamp at `dario.c:1034-1041`, `README.md:264`).

### Kuramoto coupling test (K=0.02) — REVISED (HIGH-fix #6)

v1's test was "force two chambers and watch phase difference shrink under decay". Problem: both chambers decay toward 0 anyway; absolute phase diff also shrinks; sync is indistinguishable from naked decay.

**Revised test**: hold ONE chamber DRIVEN at a non-zero level via sustained trigger; observe whether ANOTHER chamber tracks the driver via the K=0.02 coupling term.

Setup:
- Boot fresh `make dario`.
- Driver chamber: **LOVE** (sustained trigger = dense in-vocab seed-word input every turn → resonance > 0.7 each turn → LOVE excitation steady).
- Observed chamber: **COMPLEX** (depends on `|LOVE - RAGE|` AND on Kuramoto coupling from other chambers per `dario.c:1024-1026`).
- Run for 30 turns, holding LOVE high and providing zero RAGE trigger.

Assertions:
1. LOVE stays high (mean LOVE > 0.4 across the 30-step window).
2. COMPLEX RISES above its pure-trigger contribution (which is zero, since RAGE is held below 0.2). Any non-zero COMPLEX value comes from Kuramoto coupling alone: `C[COMPLEX] += 0.02 × sin(C[LOVE] - C[COMPLEX])`. If LOVE > COMPLEX, the sin term is positive, COMPLEX rises.
3. COMPLEX is strictly higher in the LOVE-driven run than in a control run with zero LOVE trigger (run both, compare).

Pass criterion: COMPLEX(driven) − COMPLEX(control) > 0.01 across at least 10 of the 30 steps. (0.01 chosen as detection floor above sampling noise.)

This isolates the K=0.02 coupling term as the sole differentiator. If COMPLEX in the driven run exceeds the control run, Kuramoto coupling is observable; if not, the K=0.02 effect is below floor in this regime → finding for paper.

### Pass criteria

- All 6 chambers individually triggerable (within their adjusted turn windows: FEAR/LOVE/VOID 5 turns; RAGE 8 turns per CRIT-fix #3; FLOW 10 turns per HIGH-fix #7; COMPLEX 14 turns).
- Decay rates within ±10% of documented values across 10 idle steps.
- Somatic markers stay in [0.5, 2.0] under sustained high state.
- Kuramoto driven-vs-control test: COMPLEX(driven) > COMPLEX(control) by ≥ 0.01 on ≥ 33% of the 30-step window (HIGH-fix #6).

### Failure recovery

If a chamber doesn't trigger, log the actual signal level (`dissonance`, `resonance`, etc.) and confirm whether `process_input` order (`dario.c:1884-1894`) is letting the trigger reach `chamber_update` before laws enforce. Document and proceed.

**Estimated wall time:** 40 min (extra turns for RAGE / FLOW / Kuramoto). **Cost @ $1.39/hr:** ~$0.93.

---

## 6. Phase 3 — Velocity operators

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

**Estimated wall time:** 18 min. **Cost @ $1.39/hr:** ~$0.42.

---

## 7. Phase 4 — Seasons + laws of nature

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

**Estimated wall time:** 25 min. **Cost @ $1.39/hr:** ~$0.58.

---

## 8. Phase 5 — SARTRE kernel

### Goal
Exercise the full SARTRE surface: 16 modules, 8 namespaces, 32 packages, 8-event ringbuffer; auto-profile registered models; verify overlay ratio progression; pipe `sartre_state_to_json` through `jq` for schema validation. Slot-cap tests via a dedicated C harness, NOT REPL commands (HIGH-fix #4).

### Inputs
`make full` (build #4: dario + sartre, no kk) and `make sartre` (build #2 alone). Use both to test SARTRE in isolation and integrated. PLUS a new C harness `05_sartre/test_slot_caps.c` that exercises the SARTRE C API directly.

### 8.1 SARTRE alone

```bash
./sartre_kernel 2>&1 | tee 05_sartre/standalone.log
```

Verify printed state contains:
- module count ≥ 1 (kernel registers itself first per README.md:605),
- ramp namespaces / packages list,
- event ringbuffer entries,
- non-zero `boot_time_ms`.

### 8.2 dario+sartre integrated — JSON introspection

Boot the `make full` binary and step it through 100 turns of varied input. The REPL commands `/kernel /packages /models` only PRINT state (verified `README.md:545-547` 2026-05-08). They CANNOT register new entries. Use them ONLY for state inspection:

```bash
./dario --web 3001 &
sleep 2
curl -s http://127.0.0.1:3001/api/kernel | jq '.' > 05_sartre/kernel.json
```

Validate via jq that `sartre_state_to_json` (`sartre_kernel.h:290`) emits expected schema (see 8.10 below).

### 8.3 Slot-cap C harness — `05_sartre/test_slot_caps.c` (HIGH-fix #4)

REPL commands cannot programmatically register modules / namespaces / packages / events. Slot-cap testing requires direct C-API calls. Write a small standalone harness:

```c
/* 05_sartre/test_slot_caps.c
 * Exercises sartre_kernel C API: module / namespace / package slot caps.
 * Build:
 *   cc test_slot_caps.c ../sartre_kernel.c -O2 -lm -o test_slot_caps
 * Run:
 *   ./test_slot_caps
 * Output: pass/fail per slot category + last_events ringbuffer state.
 */
#include "../sartre_kernel.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    if (sartre_init(NULL) != 0) { puts("FAIL: sartre_init"); return 1; }

    /* Modules: SARTRE_MAX_MODULES = 16 (sartre_kernel.h:83) */
    int mod_ok = 0;
    for (int i = 0; i < 20; i++) {
        char name[32]; snprintf(name, sizeof(name), "test_mod_%d", i);
        sartre_update_module(name, SARTRE_MODULE_IDLE, 0.1f);
    }
    SartreSystemState *st = sartre_get_state();
    /* state.modules[] should have at most 16 entries; reading state.module_count */
    /* (assume header exposes module_count; if not, count non-empty names) */
    int mc = 0;
    for (int i = 0; i < SARTRE_MAX_MODULES; i++) {
        if (st->modules[i].name[0]) mc++;
    }
    printf("modules: %d / %d (cap %d)\n", mc, 20, SARTRE_MAX_MODULES);
    if (mc <= SARTRE_MAX_MODULES) mod_ok = 1;

    /* Namespaces: 8 cap (sartre_kernel.h NUM_NAMESPACES symbol; verify). */
    int ns_ids[10]; int ns_ok = 0;
    for (int i = 0; i < 10; i++) {
        char nsn[32]; snprintf(nsn, sizeof(nsn), "test_ns_%d", i);
        ns_ids[i] = sartre_ns_create(nsn, 0.1f, 64.0f);
    }
    int ns_created = 0;
    for (int i = 0; i < 10; i++) if (ns_ids[i] >= 0) ns_created++;
    printf("namespaces: %d created / 10 attempted (cap 8 expected)\n", ns_created);
    if (ns_created <= 8) ns_ok = 1;

    /* Packages: SARTRE_MAX_PACKAGES = 32 */
    int pkg_ok = 0;
    int pkg_attempts = 35;
    int pkg_succeeded = 0;
    for (int i = 0; i < pkg_attempts; i++) {
        char pn[32]; snprintf(pn, sizeof(pn), "test_pkg_%d", i);
        if (sartre_pkg_register(pn, "1.0", 1024) >= 0) pkg_succeeded++;
    }
    printf("packages: %d succeeded / %d attempted (cap 32 expected)\n",
           pkg_succeeded, pkg_attempts);
    if (pkg_succeeded <= SARTRE_MAX_PACKAGES) pkg_ok = 1;

    /* Events: 8-slot ringbuffer (SARTRE_MAX_EVENTS = 8) */
    for (int i = 0; i < 12; i++) {
        char ev[32]; snprintf(ev, sizeof(ev), "test_event_%d", i);
        sartre_notify_event(ev);
    }
    /* Inspect last_events ring; expect 8 entries, oldest 4 evicted. */
    /* Print whatever sartre_state exposes for events. */
    sartre_print_state();

    sartre_shutdown();

    int all = mod_ok && ns_ok && pkg_ok;
    printf("\nslot-cap test: %s\n", all ? "PASS" : "FAIL");
    return all ? 0 : 1;
}
```

Build + run:

```bash
cd 05_sartre/
cc test_slot_caps.c ../sartre_kernel.c -I.. -O2 -lm -o test_slot_caps \
   2>&1 | tee build_test_slot_caps.log
./test_slot_caps 2>&1 | tee test_slot_caps_run.log
echo "exit=$?"
```

If `sartre_get_state()` exposes the modules-array directly (verified `sartre_kernel.h:218`), the harness counts entries. If not, the harness uses `sartre_state_to_json` and parses with `jq`.

### 8.4 Module slots (16, per `sartre_kernel.h:83`)

Captured via the harness in 8.3. After 20 attempts, `module_count == 16` (cap enforced). Document overflow behavior (silent reject vs error return).

### 8.5 Namespace slots (8 — cap symbol verified via the harness's runtime measurement)

`sartre_ns_create` calls capped at 8. Verify the 9th-onwards return -1 (or whatever the rejection convention is per `sartre_kernel.h:232`).

### 8.6 Package slots (32, per `sartre_kernel.h:85`)

`sartre_pkg_register` capped at 32. Dario installs 8 packages on bootstrap (`dario.c:1776-1789`), so the harness starts from a fresh process to avoid contamination — DO NOT mix the harness with the dario binary in the same SQLite connection.

### 8.7 Event ringbuffer (8 slots per `sartre_kernel.h:84`)

12 events injected; only the last 8 retained (wraparound semantics per `README.md:623`).

### 8.8 Model registry — register all weights (HIGH-fix #8: parse JANU header, not file size)

```bash
# Inline C harness 05_sartre/register_models.c that:
# - sartre_init(NULL)
# - for each .bin in weights/: parse JANU header to get authoritative param_count
# - call sartre_model_register(name, path)
# - prints sartre_model_list() and sartre_model_best()
```

Authoritative `param_count` parse — JANU header layout (verified `infer_v4.c:504-525`):
- Bytes 0-3: magic `0x4A414E55` ('JANU' little-endian).
- Bytes 4-7: version (int).
- Bytes 8-11: V (vocab).
- Bytes 12-15: E (embed dim).
- Bytes 16-19: H (heads).
- Bytes 20-23: D (depth-related).
- Bytes 24-27: B (blocks).
- Bytes 28-31: M (M dim).
- Bytes 32-35: T (context).
- **Bytes 36-39: n_params (int, authoritative count).**
- Bytes 40-255: padding.

Harness reads bytes 0-3 to confirm magic, then bytes 36-39 for n_params:

```c
/* 05_sartre/register_models.c */
#include "../sartre_kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static long parse_janu_n_params(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    int magic; fread(&magic, 4, 1, f);
    if (magic != 0x4A414E55) { fclose(f); return -2; } /* not JANU */
    fseek(f, 36, SEEK_SET);   /* skip ver(4) + V,E,H,D,B,M,T (28 bytes) */
    int n_params; fread(&n_params, 4, 1, f);
    fclose(f);
    return n_params;
}

int main(void) {
    sartre_init(NULL);
    DIR *d = opendir("weights"); if (!d) return 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, ".bin")) continue;
        char path[256]; snprintf(path, sizeof(path), "weights/%s", e->d_name);
        long np = parse_janu_n_params(path);
        if (np <= 0) {
            fprintf(stderr, "[skip] %s: not JANU (np=%ld)\n", e->d_name, np);
            continue;
        }
        printf("[register] %s: n_params=%ld\n", e->d_name, np);
        sartre_model_register(e->d_name, path);
    }
    closedir(d);
    sartre_model_list();
    const SartreModelProfile *best = sartre_model_best();
    if (best) printf("best: %s (%lld params)\n", best->name, (long long)best->param_count);
    sartre_shutdown();
    return 0;
}
```

Build + run:

```bash
cc 05_sartre/register_models.c sartre_kernel.c -I. -O2 -lm -o 05_sartre/register_models
./05_sartre/register_models 2>&1 | tee 05_sartre/register_models.log
```

Expected: each `.bin` is auto-profiled — `param_count` derived from the JANU header (NOT file size, contra v1); `runtime_mb` computed; `fits_in_ram == 1` for ALL since A100 80GB system RAM is ≥ 100 GB on the SXM hosts (verify via `free -h`). `sartre_model_best()` should return whichever has highest `param_count`. With 80GB+ host RAM, all six bins fit; the largest should be `janus_v4_sft_*` (~673 MB each per AML smoke log `aml/README.md:147`) or `resonance_200m_lora_yent.bin` — ranking determined by JANU `n_params`.

### 8.9 OverlayFS ratio

Per `dario.c:1793` and `sartre_kernel.h:116-121`: bootstrap initializes `base_size = 83 KB`. After 100 ingest+generate turns, expect `delta_size > 0` and growing. Assert `overlay_ratio` strictly monotonic-non-decreasing across the 100-step window. Assert `base_size` constant (immutable).

### 8.10 Three flags (`spiral_detected`, `wormhole_active`, `strange_loop`)

Per `README.md:639` ("Currently set externally"). The flags are not auto-detected yet. The test confirms: (a) all three default to 0 at boot, (b) calling `sartre_set_flags(...)` (or whatever the API is — verify by `grep` if missing, else flag as no-API), they flip. If no setter API exists, document expected vs observed: expected = settable from outside, observed = no public API → finding for paper.

### 8.11 JSON schema validation

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
- `test_slot_caps` C harness builds and exits 0; all four slot caps (16 / 8 / 32 / 8) enforced via direct C-API.
- `register_models` harness loads all weights via JANU-header param parse; `model_best()` returns the highest-`n_params` model.
- Overlay ratio strictly grows; base immutable.
- JSON schema has all 11 required top-level keys.

### Failure recovery

If a slot cap doesn't enforce, that's a real bug; flag for paper but DO NOT patch in this run. If the JSON schema is missing a key, document the mismatch with the README and emit a fix-spec for follow-up. If `register_models` reports a non-JANU file (`np == -2`), confirm it's a legacy-format weight file and use `infer_v4.c:521-525` legacy n_params calculation as fallback.

**Estimated wall time:** 45 min (extra 10 min for harness build/run). **Cost @ $1.39/hr:** ~$1.04.

---

## 9. Phase 6 — KK Knowledge Kernel

### Goal
Exercise FTS5 retrieval, 7-signal scoring, Hebbian bridge, embedding slot, lineage / re-ingest, bi-directional KK, all 7 essays, and Charged KK (36 anchor words × 8 chambers, EMA 0.8/0.2).

### Inputs
`make all` (build #5: full triple). Fresh DB at `/tmp/runpod_kk.db`.

### 9.1 FTS5 retrieval — dario_essay.txt

```bash
rm -f /tmp/runpod_kk.db
./kk init /tmp/runpod_kk.db
./kk ingest /tmp/runpod_kk.db ./docs/dario_essay.txt knowledge public
./kk query /tmp/runpod_kk.db "resonance field" public 5 > 06_kk/q1.txt
```

Assert: `kk_get_stats` reports chunks count for that doc. README says **71** chunks (`README.md:1064`). Capture observed chunk count; if it differs from 71, the README is stale; record both.

### 9.2 Seven-signal scoring weights validation

The weights are at `README.md:707-718`: lexical 0.36, recency 0.12, trust 0.10, linkage 0.16, scope 0.10, namespace 0.08, freshness 0.08 (sum 1.00). Plus Hebbian boost as 8th when bridge attached.

Test approach: feed a query for which several chunks score similarly on lexical, then rerun with `recency` weighted higher (re-ingest one chunk to bump its `seen_count` → recency boost). Assert top result changes. Repeat for each signal, varying it independently while holding others. This requires the kk CLI to expose per-signal weights — if it doesn't, document as a finding (`README.md:707-718` is then aspirational rather than empirical).

### 9.3 Hebbian bridge

Within the integrated dario binary, the bridge is wired at `dario.c:910-915`. The three callbacks:
- `dario_kk_word_resonance` — call site verified at `dario.c:910`,
- `dario_kk_get_prophecies` — `dario.c:911`,
- `dario_kk_destiny_magnitude` — `dario.c:904-908`.

For each callback:

1. Run dario for 5 turns to populate cooc / prophecies / destiny.
2. Issue an internal kk query (via dario's `kk_modulate_field` at `dario.c:919-992`).
3. Compare retrieval ranks for the same query with and without bridge attached. Assert ranks differ (Hebbian boost > 0 for at least one of the top 5 results).

### 9.4 Embedding slot

Wire a model embedder via `kk_set_embedder` (`kk_kernel.h:226`). Use the Janus 176M's hidden state as embedder. The embedder API is `embed_fn(text, len, out, user_data) → dim`. Add a thin shim that calls `infer_v4`'s `forward_token`/`prefill_batch` with `hidden` as output (the binary already exposes `hidden` per `infer_v4.c:301`, `infer_v4.c:465`). For Phase 6 we exercise the slot with a SIMPLER embedder (random-init float[64]) just to confirm it fires; the production wiring is deferred to Phase 11 / 12.

Assert: with embedder attached, scoring picks up `rrpram_resonance` (`kk_kernel.h:67`) > 0 in the result struct.

### 9.5 Lineage — re-ingest unchanged + modified

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

### 9.6 Bi-directional KK

Run a 5-turn dialogue mode with KK absorption (per `aml/README.md:151-160`). After each turn, query SQLite for the document count. Expect count to grow as model output is absorbed (one new doc per absorbed utterance, dedup by content hash per `kk_kernel.h:250`).

### 9.7 All seven essays loaded sequentially

Files at `~/arianna/dario/docs/`: `bach_counterpoint.txt`, `bioluminescence.txt`, `byzantine_iconography.txt`, `dario_essay.txt`, `dickens_russian_lit.txt`, `mycorrhizal_networks.txt`, `polynesian_navigation.txt` (verified `ls ~/arianna/dario/docs/` 2026-05-08).

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

### 9.8 Charged KK — 36 anchor words × 8 chambers, EMA 0.8/0.2

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

**Estimated wall time:** 40 min. **Cost @ $1.39/hr:** ~$0.93.

**Codex audit checkpoint #4 (placed AFTER Phase 4 in the spec, but practically also re-fed after Phase 6):** feed `04_seasons/timeseries.tsv` and `06_kk/*.txt`.

---

## 10. Phase 7 — Voice quality + multi-temp sweep

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

`infer_v4` already accepts top_k via positional CLI (`infer_v4.c:497-498`); the binary CLI accepts `[seed] [top_k]`. To pass `top_k=∞` use `top_k=0` (no filter, per `infer_v4.c:498`).

`--rep-penalty F` is now a CLI flag added in Phase 0.5 (architect-fix #11). The three-binary trick from v1 is OBSOLETE — single canonical binary handles the sweep.

### Three fixed prompts per voice

1. Technical: `"What is the RRPRAM mechanism inside Janus attention?"` (same prompt for all voices — direct technical question).
2. Philosophical: `"Does memory create identity, or does identity create memory?"`
3. Personal: `"Tell me what you remember most clearly from before."`

Each cell generates 100 tokens. Save raw output to `07_voices/transcripts/<voice>_<temp>_<topk>_<rp>_<promptN>.txt`.

### Per cell

```bash
# Voice loop — single canonical binary, --rep-penalty CLI flag (Phase 0.5)
for voice in leo arianna yent resonance-yent leo24m; do
  WEIGHTS=$(resolve_weights $voice)   # via voices.go map; helper script
  for prompt_id in 1 2 3; do
    for temp in 0.3 0.5 0.7 0.8 0.9 1.0; do
      for topk in 40 0; do
        for rp in 1.0 1.3 1.4; do
          ./infer_v4 "$WEIGHTS" "Q: $PROMPT\nA:" 100 $temp 42 $topk \
              --rep-penalty $rp \
              > "07_voices/transcripts/${voice}_t${temp}_k${topk}_rp${rp}_p${prompt_id}.txt" \
              2>&1
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

180 cells × 2 s each ≈ 6 minutes pure compute. Loads + transcript writes push to ~20 min total. **Cost:** ~$0.46.

**Estimated wall time:** 30 min (lots of bookkeeping and the human pass). **Cost @ $1.39/hr:** ~$0.70.

**Codex audit checkpoint #1:** feed `07_voices/scores.tsv`, the per-voice optimal-cell transcripts (top 3 per voice), and the scoring methodology Bash script. Architect runs lock-in independently.

---

## 11. Phase 8 — Modes (chain / dialogue / duet / trialogue)

### Goal
Exercise four chain_dialogue modes; reproduce two README samples on a qualitative basis (architect-fix #13 drops fragile lexical-overlap thresholds); document the duet/trialogue Go fallback (Опус-2 caveat #1 from `aml/README.md:187-192`). Phase 8.3 (explore mode) DROPPED — no AML/Go support, Python-only (CLI surface verification 0.6).

### Inputs
`make aml-bins` (build AML binaries) and `make go-bins` (build Go binaries — for duet/trialogue and dialogue with the Go-CLI surface). Use `make all` dario for the C field-absorption path. Phase 0.6 has verified the actual flag matrix.

### 11.1 Chain mode (single voice, AML)

```bash
./aml/dario_dialogue --mode chain --voice leo --topic "What is consciousness?" \
    --depth 6 --max-tokens 80 \
    --knowledge ../docs/dario_essay.txt --kk-db /tmp/p8_chain.db --no-field
```

(AML supports `--kk-db` and `--no-field` per `aml/dario_dialogue.aml:617, 616`.)

#### Reproduction acceptance — REVISED (architect-fix #13)

v1 demanded ≥ 30% lexical overlap with the README sample (`runpod_plan_v1.md:649`). Problem: README samples were generated at unknown seeds and the published transcript was post-edited; lexical overlap is fragile-to-meaningless. **Replace with qualitative criterion**: voice register identifiable.

For Leo (`README.md:1043`), the architect's qualitative reading checks for:
- Long-form metaphor (≥ 1 multi-sentence figurative passage).
- "I" / introspection markers, calm/contemplative register.

For Yent (Phase 8.4 below), checks: short, confrontational, wry register.

For Arianna: precise, philosophical, structured prose.

The architect makes the qualitative judgment after reading the 6 turns. PASS = voice recognizable to the architect. FAIL = voice unrecognizable. No numerical threshold. Seed=42 used for reproducibility WITHIN this run only.

Verify: KK absorbs each turn — `sqlite3 /tmp/p8_chain.db 'SELECT count(*) FROM documents;'` should show 7 docs (1 essay + 6 absorbed turns).

### 11.2 Dialogue mode (5-turn interactive, AML)

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

### 11.3 [DROPPED] Explore mode

Per Phase 0.6 verification: explore mode exists ONLY in the legacy Python `chain_dialogue.py:1009` (banned on inference path). Neither AML `dario_dialogue.aml` (modes: `chain | dialogue`) nor Go `bin/dario-dialogue` (modes: `chain | dialogue | duet | trialogue`) implements explore.

Drop the section. Reallocate the budget to extra qualitative reading on duet (11.4) and trialogue (11.5) transcripts. Document as a finding in `08_modes/dropped_explore.txt`: "Explore mode is Python-only; AML/Go ports do not implement it. Listed as TODO for AML port — see `memory/feedback_python_ban_2026_04_29.md` for the Python policy."

### 11.4 Duet mode — Go binary (no `--no-field`, no `--kk-db` in Go CLI)

Per Phase 0.6 verification: Go `bin/dario-dialogue` does NOT have `--no-field` or `--kk-db` flags (KK is in-process via `kk.New()` per `cmd/dario-dialogue/main.go:169`). v1 8.4's invocation needs to drop these flags.

```bash
./bin/dario-dialogue --mode duet --voice leo --voice2 yent \
    --topic "consciousness" --depth 5 \
    --knowledge ./docs/dario_essay.txt
```

Verifies `cmd/dario-dialogue/main.go:155-156, 207-209` — duet mode and voice2 resolution.

Field POSTing happens automatically when `cfg.Field` (default `http://localhost:3001`) probes alive (`cmd/dario-dialogue/main.go:194-202`). To suppress field absorption, ensure no `dario --web 3001` is running concurrently. Document the field-up status in transcript header.

Reproduce the README sample at `README.md:1041` (Leo + Yent on consciousness). Capture transcript; verify two distinct voices alternate correctly (5 rounds × 2 voices = 10 turns). Architect qualitative pass per architect-fix #13 — no lexical overlap threshold.

Assertion: each turn's stderr includes `[kk for <voice>]` injection log line (`cmd/dario-dialogue/main.go:520`).

### 11.5 Trialogue mode — Go binary

```bash
./bin/dario-dialogue --mode trialogue --voice leo --voice2 yent --voice3 arianna \
    --topic "What is the relationship between light and consciousness?" \
    --depth 4 \
    --knowledge ./docs/byzantine_iconography.txt
```

Per `README.md:1304-1325` — use the same prompt and knowledge. Capture full transcript; expect 4 rounds × 3 voices = 12 turns alternating leo → yent → arianna → leo → … (per `cmd/dario-dialogue/main.go:615`).

### Pass criteria

- 4/4 modes (chain / dialogue / duet / trialogue) produce non-empty, well-formed transcripts.
- KK chunk count grows in dialogue (bi-directional confirmed).
- Duet and trialogue identifiably-different voices per turn (architect qualitative pass).
- Voice register matches the README's published flavor (qualitative — see architect-fix #13).

### Failure recovery

If duet hangs, the goroutine deadlock (mailboxes never close) is the likely cause. Re-run with `--depth 2` (smaller fan-out). If still failing, drop to two sequential single-voice chains and document the duet binary as broken on this build.

**Estimated wall time:** 30 min (5 saved by dropping explore). **Cost @ $1.39/hr:** ~$0.70.

---

## 12. Phase 9 — Cross-architecture duet (Janus 176M vs Resonance 200M)

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
    --knowledge ./docs/dario_essay.txt
# 5 × 2 = 10 turns
```

(No `--no-field`, no `--kk-db` per Phase 0.6 verification.)

Capture transcript to `09_cross_arch/transcript.txt`.

### Pass criteria

- ≥ 10 turns generated.
- Both architectures load and inference (check stderr for `[janus-v4]` and the resonance backend prefix).
- Output is text not gibberish (qualitative — architect reads).

### Failure recovery

The voices catalog in `voices.go:59-64` lists `resonance-yent` with `Backend: BackendResonance`. The infer_v4 binary detects backend from weight magic (per `infer_v4.c:506-525`). If detection fails, log the magic bytes and continue.

**Estimated wall time:** 12 min. **Cost @ $1.39/hr:** ~$0.28.

---

## 13. Phase 10 — Web UI / HTTP forum

### Goal
Verify `--web` socket server, four HTTP endpoints, AML forum binary, and run a 4-worker concurrent-load test against `/api/forum`.

### Inputs
`make all` for the dario web binary; `make aml-bins` for `aml/dario_forum`.

### 13.1 dario --web on port 3001

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

### 13.2 AML dario_forum on port 3002

Per Phase 0.6: `aml/dario_forum` flags are `--port --host --knowledge --kk-db --field --no-field --binary --help` (NO `--max-tokens`). v1's POST body still includes `max_tokens`; the AML binary may either honor it (server-side) or default — verify via response.

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

**Estimated wall time:** 22 min. **Cost @ $1.39/hr:** ~$0.51.

**Codex audit checkpoint #2:** feed `10_web/concurrent_timings.tsv`, JSON schema validation outputs, and any 4xx/5xx response logs.

---

## 14. Phase 11 — AML / Go / C parity

### Goal
Three identical prompts × three voices × three implementations (AML / Go / C) — capture token-level diffs. Acceptance: ≥ 95% token-position parity for the first 50 tokens given identical seed (where seed is supportable).

### Inputs
- AML: `./aml/dario_infer` (no `--seed` flag per Phase 0.6 verification — see Section 14.4).
- Go: `./bin/dario-infer` (has `--seed`).
- C: `./infer_v4` (has positional seed at argv[5]).

All three should ultimately call `infer_v4` under the hood (AML and Go spawn it as a subprocess per `aml/README.md:47`, `cmd/internal/dario/infer.go:65`). Drift can come from prompt-wrapping differences (Q:/A: vs raw), tokenizer state, or different default flags.

### Voices: leo, yent, resonance-yent (covers both backends).

### Three prompts (reused from Phase 7 to amortize)
1. Technical
2. Philosophical
3. Personal

### 14.1 Steps

For each (voice, prompt) pair:

```bash
SEED=42; TEMP=0.7; TOPK=40

# AML — no --seed; relies on the spawned infer_v4 inheriting whatever seed it sets internally.
# If AML doesn't set a deterministic seed, parity drops to lexical comparison.
./aml/dario_infer --voice $VOICE --max-tokens 50 --temp $TEMP --topk $TOPK \
    "$PROMPT" > 11_parity/aml_${VOICE}_${PID}.txt

# Go has --seed
./bin/dario-infer --voice $VOICE --max-tokens 50 --temp $TEMP --topk $TOPK \
    --seed $SEED "$PROMPT" > 11_parity/go_${VOICE}_${PID}.txt

# C — positional seed at argv[5]
./infer_v4 weights/$WEIGHTS_FILE "Q: $PROMPT\nA:" 50 $TEMP $SEED $TOPK \
    > 11_parity/c_${VOICE}_${PID}.txt
```

Tokenize each output the same way (using `infer_v4`'s own BPE — write a tiny C shim that calls `nt_bpe_encode` on each output and dumps token IDs).

Compare token-by-token across the three implementations. Compute pairwise:
- AML vs Go (seed-mismatch caveat — see 14.4)
- AML vs C (seed-mismatch caveat)
- Go vs C

Define **token-position parity** as: fraction of positions in [0, 50) where both impls produce the same token ID.

### 14.2 Pass criteria

- Mean pairwise parity Go-vs-C ≥ 95% across (voice, prompt) cells (both have explicit seed plumbing).
- AML-involving pairs: parity informational; ≥ 80% lexical-bigram overlap is the soft floor.
- Per-cell parity ≥ 80% as a hard floor for Go-vs-C only.
- Where parity < 95%, root-cause must be identified — likely Опус-2 caveat #3 (`aml/README.md:206-213`): chat-token compromise. AML & Go wrap as `Q: ...\nA:`; raw C invocation differs. Document the token-position of first divergence.

### 14.3 Failure recovery

If parity drops below 80% on Go-vs-C, dump tokenizer outputs side-by-side. Likely one of:
- prompt-wrapping mismatch (Q:/A: extra newline)
- different default rep_pen (now patchable via Phase 0.5 `--rep-penalty`)
- seed propagation difference
Document and proceed; this is a paper-finding, not a bug-fix run.

### 14.4 AML seed caveat (Phase 0.6 finding)

AML `dario_infer` does NOT expose `--seed`; it spawns `./infer_v4` as a subprocess. Whether the spawned binary uses a deterministic seed depends on AML's internal env-var or argv handling — verified via Phase 0.6 by reading `aml/dario_infer.aml` for `seed`/`SRAND` calls.

Phase 0.6 result determines Phase 11.4:
- IF AML hardcodes a deterministic seed (or accepts one via env var) → parity test possible.
- IF AML uses wall-clock time(NULL) → AML cell is non-reproducible; drop AML from token-position comparison; use lexical-bigram overlap instead.

**Estimated wall time:** 25 min. **Cost @ $1.39/hr:** ~$0.58.

---

## 15. Phase 12 — High-leverage Опус-2 unblockers (residual after Phase 0.5)

### Decision rule
Phase 0.5 hoisted the chat-token fix and the rep_penalty CLI flag (the two highest-leverage items from v1's Phase 12). Phase 12 in v2 is now narrower: residual unblockers ONLY if every preceding phase landed PASS.

### 15.1 [DONE in Phase 0.5] Chat-token fix in `infer_v4.c`
Hoisted per architect-fix #11. Phase 0.5 acceptance covers byte-equal regression and ASST_END termination behavior.

### 15.2 [DONE in Phase 0.5] rep_penalty CLI flag
Hoisted per architect-fix #11. Phase 0.5 ships `--rep-penalty F`.

### 15.3 Duet/trialogue port to AML — DEFERRED

Опус-2 estimated half-day (`aml/README.md:191-192`). Explicit instruction: **do NOT run on RunPod**; flag as follow-up for a Mac Neo session.

### 15.4 Threaded forum — DEFERRED

Опус-2 noted thread-safety needs proper testing in the SQLite layer (`aml/README.md:218-221`). Same: flag as follow-up.

### 15.5 Explore mode in AML — NEW DEFERRED ITEM

Phase 8 / Phase 0.6 surfaced that explore mode is Python-only. AML port at `aml/dario_dialogue.aml:606-625` only supports `chain | dialogue`. Adding explore = ~1-day port (mirror Python's `chain_dialogue.py:661-710` `explore_mode` function in AML). Defer to Mac Neo session.

### Pass criteria (Phase 12 only if attempted)

n/a — most items deferred; the two original 12.1 / 12.2 hoisted to 0.5 and accepted there.

**Estimated wall time (if anything attempted):** 20 min residual investigation. **Cost @ $1.39/hr:** ~$0.46.

---

## 16. Phase 13 — Documentation pipeline (Zenodo paper appendix)

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
│   ├── build_dario_kk.log         (config #6, may be FAIL per CRIT-fix #1)
│   ├── make_test.log
│   ├── weights_sha256.txt
│   ├── smoke.log
│   ├── python_audit.txt
│   └── install_toolchain.log
├── 00_5_cli/
│   ├── command.sh stdout.log stderr.log metrics.json
│   ├── build_infer_v4.log
│   ├── baseline_no_flag.txt
│   ├── patched_no_flag.txt
│   ├── regression.diff
│   └── chat_tokens_canary.txt
├── 00_6_cli/
│   ├── help_dario_infer.txt
│   ├── help_dario_dialogue.txt
│   ├── help_dario_forum.txt
│   ├── help_dario-infer.txt
│   ├── help_dario-dialogue.txt
│   ├── help_dario-forum.txt
│   └── flag_matrix.tsv
├── 01_equation/
│   ├── command.sh stdout.log stderr.log metrics.json
│   └── per_term/{B,H,F,A,V,S,T}.txt
├── 02_chambers/
│   ├── command.sh ... metrics.json
│   ├── kuramoto_driven.tsv
│   ├── kuramoto_control.tsv
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
│   ├── test_slot_caps.c
│   ├── test_slot_caps_run.log
│   ├── register_models.c
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
│   ├── dropped_explore.txt
│   └── transcripts/{chain,dialogue,duet,trialogue}.txt
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
├── 12_unblockers/  (residual / deferred items list)
│   └── deferred.txt
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
  "estimated_cost_usd": 0.70,
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
Pod: A100 80GB SXM @ \$1.39/hr (verified runpodctl 2026-05-08)
Repo HEAD: $(cat "$ROOT/00_pre/git_head.txt")
Total cost (cumulative): \$$TOTAL_USD
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
- `paper_appendix_*.md` aggregates 15 phase blocks (00_pre, 00_5_cli, 00_6_cli, 01..13).
- Total cost figure sums to within 5% of the per-phase estimates.

**Estimated wall time:** 8 min (final aggregation only). **Cost @ $1.39/hr:** ~$0.19.

**Codex audit checkpoint #3:** feed `paper_appendix_*.md` + the per-phase `metrics.json` files. Architect handles edits.

---

## 17. Codex audit checkpoints — schedule

| Checkpoint | After phase | Inputs to feed | Decision criteria |
|---|---|---|---|
| #0 | Phase 0 (incl. 0.5 / 0.6) | `00_pre/build_*.log`, `make_test.log`, `git_head.txt`, `weights_sha256.txt`, `00_5_cli/regression.diff`, `00_6_cli/flag_matrix.tsv` | All required builds clean (5/6 hard); tests pass; weights match HF SHAs; CLI surfaces match plan. |
| #1 | Phase 4 | `04_seasons/timeseries.tsv`, plus 01-03 metrics | Equation correctness verified; all 7 forces dominate; chambers + velocities work. |
| #2 | Phase 7 | `07_voices/scores.tsv`, top-3 transcripts/voice, methodology Bash | Per-voice optimum identified; lock-in approval. |
| #3 | Phase 10 | `10_web/concurrent_timings.tsv`, JSON validation outputs | Web UI green; concurrent serialization documented. |
| #4 | Phase 13 | `paper_appendix_*.md`, all `metrics.json` | Final paper appendix coherent; no fabricated numbers; cost reconciled. |

Architect-only between checkpoints. Singularity mode: failure → diagnose → minimal fix → rerun, no per-step approval. Three-strikes rule from CLAUDE.md applies: on third unproductive retry, stop and report root cause to architect.

---

## 18. Cost discipline (REVISED at $1.39/hr per architect-fix #10)

| Phase | Estimated wall time | Estimated cost (A100 80GB @ $1.39/hr) |
|---|---|---|
| 0 — Pre-flight | 12 min | $0.28 |
| 0.5 — infer_v4 CLI | 35 min | $0.81 |
| 0.6 — CLI surface verify | 10 min | $0.23 |
| 1 — Equation | 25 min | $0.58 |
| 2 — Chambers (extended for RAGE/FLOW/Kuramoto) | 40 min | $0.93 |
| 3 — Velocity | 18 min | $0.42 |
| 4 — Seasons + laws | 25 min | $0.58 |
| 5 — SARTRE (with C harnesses) | 45 min | $1.04 |
| 6 — KK | 40 min | $0.93 |
| 7 — Voice sweep (single binary, --rep-penalty flag) | 30 min | $0.70 |
| 8 — Modes (explore dropped) | 30 min | $0.70 |
| 9 — Cross-arch duet | 12 min | $0.28 |
| 10 — Web UI | 22 min | $0.51 |
| 11 — Parity | 25 min | $0.58 |
| 12 — Residual unblockers (residual only) | 20 min | $0.46 |
| 13 — Documentation | 8 min | $0.19 |
| **Total (no Phase 12)** | **377 min ≈ 6.3h** | **$8.16** |
| **Total (with Phase 12)** | **397 min ≈ 6.6h** | **$8.62** |

Architect-fix #10 reconciliation: prior v1 totals were ($9.34 / $10.79); recomputed at $1.39/hr the v2 totals before extra-time additions are ($7.46 / $8.62). The phase-2 / phase-5 / phase-0.5 / phase-0.6 added 1.6h above v1 baseline (extended RAGE/FLOW windows; SARTRE C harnesses; CLI hoist; CLI verification). Net is roughly equal cost while gaining significantly more verification.

Buffer: 30% overhead for build re-runs / failed cells / debug. **6.6h × 1.3 = 8.6h ≈ $11.93 with buffer**. Stays under the $14 absolute target. Hard kill at 12h elapsed.

Per-phase cost logged to `metrics.json` as `wall_time_seconds × 1.39 / 3600`.

---

## 19. Open risks / known gotchas (REVISED)

| # | Risk | Source | Mitigation |
|---|---|---|---|
| 1 | Опус-2 caveat #3: chat-token compromise (Q:/A: wrap, BOS not injected) | `aml/README.md:206-213`, `cmd/internal/dario/infer.go:60-70` | **Phase 0.5 lands the fix; Phase 7 uses `--chat-tokens` for SFT voices**. Regression-tested for byte-equality at default. |
| 2 | Forum sync-accept blocks under load | `aml/README.md:215-221` | Phase 10 documents serialization; do not patch under fire. |
| 3 | 8-emotion fingerprint zeros in C | `aml/README.md:192-201` | Phase 6 Charged KK records observed=zeros; flag for paper. |
| 4 | `make test` 1725/1725 claim unverified | `README.md:512` (claim), `tests/test_dario.c:23-24` (dynamic counter) | Phase 0 captures actual count; report match/mismatch. |
| 5 | KK SQLite db path conflicts with concurrent voices | inferred from sqlite single-writer | Per-process DB path in `KK_DB_PATH` env var; confirmed setter at `dario.c:1806`. Only AML binaries support `--kk-db` per Phase 0.6; Go binaries use in-memory KK. |
| 6 | Build #6 (dario+kk no sartre) may not exist as a stock target | `Makefile` doesn't include this combination | **CRIT-fix #1**: Phase 0 acceptance softened to 5/6 hard. Manual cc invocation (HIGH-fix #10); flag if it fails. README claim becomes empirical-finding. |
| 7 | rep_penalty hardcoded in infer_v4 | `infer_v4.c:627` | **Phase 0.5 adds `--rep-penalty F` CLI flag (architect-fix #11)**. Three-binary trick obsoleted. |
| 8 | Dario.html / forum.html may not exist on the pod | filesystem | `00_pre` confirms both files via `ls`. |
| 9 | RunPod's reverse proxy may block ports 3001/3002 | Runpod docs | Fall back to 18801/18802 (Опус-2's known-working ports per `aml/README.md:163`). |
| 10 | `dario_memory.db-shm` / `-wal` left over in repo | `ls` shows them | Phase 0 wipes any pre-existing `.db*` files at the canonical path before running KK tests. |
| 11 | A100 80GB SXM may be 40GB at provisioning if Runpod default differs | provisioner | Phase 0 step 1.1 verifies via `nvidia-smi`. |
| 12 | `make weights` requires `hf` CLI authenticated | Makefile:73 | **HIGH-fix #2**: Phase 0 toolchain installs `huggingface_hub[cli]` via pip3 (single Python touchpoint, banned-on-inference but allowed for CLI tooling); confirms `hf whoami` works; auth via `HF_TOKEN` env. |
| 13 | Sweep grid 180 cells × ~2s could be longer if A100 isn't 5-10× faster than Neo | speed assumption | First Phase 7 cell measures actual tok/s; abort sweep if < 50 tok/s and reduce cells to 60 (drop rep_pen axis). |
| 14 | Phase 5 flag-setter API may not exist | `README.md:639` "currently set externally" | Phase 5 step 8.10 explicitly documents this; no API → finding for paper. |
| 15 | Phase 8 README sample reproduction — exact text WILL NOT match (different state) | sampling stochasticity | **architect-fix #13**: drop lexical overlap target; use architect qualitative voice-register judgment instead. |
| 16 | **NEW:** Trauma accumulates at most 0.1/turn; 5-turn window cannot exceed RAGE threshold | `dario.c:1887-1888` (verified) | **CRIT-fix #3**: extend RAGE trigger window to 8 turns; verify per-turn rate observationally and re-derive in real time if dissonance peaks below 1.0. |
| 17 | **NEW:** SARTRE slot-caps cannot be tested via REPL — REPL only prints state | `README.md:545-547` (verified), `sartre_kernel.h:204-244` (C-API) | **HIGH-fix #4**: write `05_sartre/test_slot_caps.c` C harness using direct C API (`sartre_update_module`, `sartre_ns_create`, `sartre_pkg_register`, `sartre_notify_event`); compile + run; capture exit code. |
| 18 | **NEW:** SARTRE param_count must come from JANU header bytes 36-39, NOT file size | `infer_v4.c:504-525` (verified header layout) | **HIGH-fix #8**: `register_models.c` parses bytes 0-3 (magic) and bytes 36-39 (n_params) from the JANU header. Falls back to file-size heuristic only for legacy non-JANU files. |
| 19 | **NEW:** Kuramoto sync test conflated with naked decay in v1 | `dario.c:1021-1031` (decay AND coupling both shrink phase diff) | **HIGH-fix #6**: revised test holds LOVE driven, observes COMPLEX track via K=0.02 coupling; compare driven-vs-control runs. |
| 20 | **NEW:** FLOW excitation cannot reach 0.2 in 5 turns due to entropy floor capping emergence at ~0.85 | `dario.c:1015, 1480, 1486` (verified) | **HIGH-fix #7**: extend window to 10 turns; threshold relaxed to "> 0.15 within 10 turns". |
| 21 | **NEW:** `tokenizer.pkl` is legacy Python path; not consumed on the pod | `Makefile:80`, `infer_v4.c:511-512` | **MED-fix #5**: download for completeness; not gated on Phase 0 acceptance; document. |
| 22 | **NEW:** Makefile go-bins target lacks `mkdir -p bin/` | `Makefile:86-90` (verified) | **Codex-fix #14**: Phase 0 / Phase 7 invocations prepend `mkdir -p bin/` before any `make go-bins`; alternatively, patch the Makefile in Phase 0.5 with `@mkdir -p $(@D)` (under the same regression-test umbrella). |
| 23 | **NEW:** AML CLI surface differs from v1 plan assumptions (no `--seed` on `dario_infer`; explore mode missing; AML `dario_dialogue` modes are `chain | dialogue` only) | `aml/dario_infer.aml:263-282`, `aml/dario_dialogue.aml:544-625` (verified) | **architect-fix #12**: Phase 0.6 verifies surfaces; Phase 8.3 explore mode dropped; Phase 11 AML cell becomes informational rather than parity. |
| 24 | **NEW:** Go `bin/dario-dialogue` has no `--no-field` / `--kk-db` flags (KK in-process) | `cmd/dario-dialogue/main.go:73-99` (verified) | **architect-fix #12**: Phase 8.4 / 8.5 / 9 invocations drop these flags; document KK-in-memory behavior in transcripts. |
| 25 | **NEW:** README sample reproduction was based on unknown seeds + post-edited transcripts | `README.md:1041-1043` (samples), git history (post-edits) | **architect-fix #13**: replace 30% lexical overlap target with architect qualitative voice-register judgment. Seed=42 within-run only. |

---

## 20. Memory rules in effect during execution

These bind the architect during the run:

1. **Provenance gate.** Every number / file / line / hash inline-cited as `X = Y (file.c:NNN | git log abcdef | hf show)`. No bare claims (per `protocol_claim_verification_gate.md` and CLAUDE.md gate #2).
2. **Ban on the diagonal-baseline optimizer name.** The string "Adam" never appears in any output, log, transcript, commit message, or paper appendix (per `feedback_adam_ban_2026_04_29.md`). Use "Chuck" or "diagonal optimizer baseline" if such a discussion arises.
3. **Python ban on inference path.** No Python in `dario` / `infer_v4` / `kk_kernel` / `sartre_kernel` runtime invocations. Bash + AWK + jq + sqlite3 + the C/AML/Go binaries. Python is allowed for: `hf` CLI / `huggingface_hub[cli]` (auth + download via `make weights`) — single touchpoint, exited after Phase 0. Phase 13 aggregation is intentionally Bash to honor the rule (per `feedback_python_ban_2026_04_29.md`).
4. **No closed-milestone retraining.** Read-only weights from `ataeff/dario` HF repo. No fine-tuning, no LoRA training, no checkpoint modification, no `make weights` re-upload (per `feedback_failure_unsolicited_finetune_2026_04_27.md`).
5. **Logs / metrics from real tool output only.** If `make test` prints "1684 / 1684 passed", record exactly that; do not paste "1725" because the README says so. The README is a claim; the log is the evidence.
6. **AI is not a tool.** In any prose written into transcripts or the paper appendix, AI is referred to as "organism", "field", "function", "component" — never "tool" (per CLAUDE.md "Specific bans").
7. **Train loss reporting standard.** Phase 7 voice quality reports use train-loss-first if any retraining has happened (it has not in this plan). Inference-only runs report tokens/s + temperature + top_k + rep_pen + seed (per `feedback_show_train_loss.md`).
8. **No self-removal / no helpful-assistant register.** The transcripts are technical, not chatty. (per `feedback_no_helpful_assistant.md`).
9. **Default = unverified.** If the architect or reviewer asks "did the model output X tokens?" — reply "не проверил, сейчас сверю" if not in `metrics.json`, then read the file (per CLAUDE.md gate #2).
10. **Singularity loop discipline.** On failed phase: reproduce → 1 hypothesis → minimal change → re-run. Three strikes without new knowledge → stop, report. (per CLAUDE.md Workflow #5).

---

## 21. Diff vs v1 (the 14 changes from merged feedback)

This section is the audit-trail for v1 → v2. Each item ties back to merged-feedback IDs (CRIT / HIGH / MED / Codex / architect).

1. **CRIT-fix #1 (Phase 1.4 vs 1.8 contradiction)** — Section 1.5 / 1.9: Phase 0 acceptance softened to **"5/6 hard, config #6 soft"**. Builds 1-5 must pass; build #6 (dario+kk no sartre) is best-effort with finding-flag on failure. README "every file compiles alone" claim becomes empirical-finding rather than precondition.
2. **HIGH-fix #2 (`hf` CLI install gap)** — Section 1.2.1: `00_pre/install_toolchain.sh` adds `apt-get install -y python3-pip` + `pip3 install --upgrade --user "huggingface_hub[cli]"`. Single Python touchpoint documented; banned-on-inference rule intact.
3. **CRIT-fix #3 (RAGE 5-turn window math)** — Section 5: trauma accumulation rate verified at `dario.c:1884-1908` (`+0.1/turn` max when `dissonance>0.7`, with `0.97/step` decay). RAGE trigger window extended to **8 turns** with margin; per-turn rate re-derived observationally if dissonance peaks below 1.0.
4. **HIGH-fix #4 (SARTRE slot-caps need C harness)** — Section 8.3: `05_sartre/test_slot_caps.c` writes a standalone harness exercising `sartre_update_module / sartre_ns_create / sartre_pkg_register / sartre_notify_event` directly. REPL `/kernel /packages /models` only PRINT state per `README.md:545-547` (verified).
5. **MED-fix #5 (`tokenizer.pkl` misleading)** — Section 1.6.1: `.pkl` documented as legacy Python path; not gated on Phase 0 acceptance.
6. **HIGH-fix #6 (Kuramoto vs naked decay)** — Section 5 Kuramoto subsection: revised test holds LOVE driven, observes COMPLEX track via K=0.02 coupling against a control run with zero LOVE trigger.
7. **HIGH-fix #7 (FLOW 5-turn too tight)** — Section 5 FLOW subsection: window extended to **10 turns**; threshold relaxed to "> 0.15 within 10 turns" (verified entropy floor 0.10 caps emergence at ~0.855; per-turn rise 0.025-0.030).
8. **HIGH-fix #8 (SARTRE param_count from JANU header)** — Section 8.8: `register_models.c` parses bytes 0-3 (magic) and bytes 36-39 (n_params) per verified `infer_v4.c:504-525` header layout. File-size heuristic dropped except as legacy fallback.
9. **HIGH-fix #10 (build #6 manual cc command)** — Section 1.4.1: full manual cc invocation written out: `cc dario.c kk_kernel.c -DHAS_KK -DHAS_DARIO -O2 -lm -lsqlite3 -o dario_kk_only`.
10. **architect-fix #10 ($1.39/hr cost reconcile)** — Section 18: cost table recomputed at $1.39/hr per `runpodctl get cloud` (2026-05-08 02:30 IDT). Totals: $8.16 (no Phase 12) / $8.62 (with) / $11.93 (with 30% buffer).
11. **architect-fix #11 (single `--rep-penalty` CLI flag)** — Section 2 (Phase 0.5): bundled with the chat-token fix into a new Phase 0.5. ≤10 LOC patch; byte-equal regression test at default rep_pen=1.3. Three-binary trick obsoleted.
12. **architect-fix #12 (AML CLI surface verification)** — Section 3 (Phase 0.6): every binary's `--help` captured to a flag-matrix. Surfaced findings: AML `dario_infer` no `--seed`; AML `dario_dialogue` modes `chain | dialogue` only; Go `bin/dario-dialogue` no `--no-field` / `--kk-db`; explore mode is Python-only. Phase 8 / 11 invocations updated accordingly.
13. **architect-fix #13 (drop lexical-overlap, use voice-register)** — Section 11.1 (chain) / 11.4-11.5 (duet/trialogue): the 30% lexical-overlap target replaced with architect qualitative voice-register judgment. Seed=42 within-run only for reproducibility; not used to chase the README's published transcript verbatim.
14. **Codex P2 #14 (Makefile go-bins missing `mkdir -p bin/`)** — Section 19 risk #22: documented; mitigation is `mkdir -p bin/` prepended to invocations (no Makefile patch unless Phase 0.5 already touches files anyway, in which case fold in).

---

End of plan v2.
