# RunPod Full-Pass Plan v2 — Dario (rebuilt code)

> Single-pod (+polygon-CPU), single-architect, **Singularity-mode** full re-run of the ENTIRE
> paper protocol on the **rebuilt** organism, to produce verified numbers for paper edition 2.
> Mirrors `runpod_plan_v3.md` (the 2026-05-08 v1 contract — 13 phases) phase-for-phase; v3 stays
> the exhaustive micro-step reference. This file is the v2 contract: full phase list, per-phase
> acceptance, rebuild-specific risk, and the **audit fixes** folded in. Plan → **Codex/Opus review
> PASS** → Singularity execution. No phase skipped.
>
> **Pinned HEADs:** rebuilt = `118fa98` (E7 — the head-to-head + laws-2000 in hand were produced
> here, NOT 78d101f); legacy = `bdacb6a` (frozen). Document build parity: both binaries built with
> identical flags, same host, same vocab=380 bootstrap — so the head-to-head attributes to the
> provenance/decoupling rebuild, not build environment.

## What changed since v1 (why re-run everything)

Force mechanisms rebuilt (token-provenance + orthogonal-feature decoupling): B,H,F,A,T now read
**input-only** accumulators, not the organism's own generation; A/V random-embedding claims retired.
Term energies changed → **every downstream result may have shifted** and must be re-measured, not
assumed. v2 carries the corrected Result 1 plus a head-to-head vs frozen legacy `bdacb6a`.

In hand, verified by adversarial recompute (re-confirm on this pod): matrix raw energies (3 platforms,
T=162.44 — but see Result-1 honesty below); head-to-head **raw-argmax** dominance (legacy A:destiny
29/40 neutral prompts vs rebuilt 0/40, B:chain 27/40); laws-of-nature full 2000-turn (β-spring 0.311,
α-summer 0.352). See `docs/dario_paper_v2_phase3_findings.md`.

## RESULT-1 HONESTY — the вошь the audit caught (binding on the whole run)

1. **Raw argmax ≠ the z-score gate.** The live `dominant_name` and the matrix diagonal are RAW
   pre-renorm energies. T's raw scale grows unbounded (162 on its trigger, ~2219 in swiglu L150,
   1975/2000 long-run). Every "dominates / leads / argmax" claim must be **labeled RAW and also
   reported under the per-force z-score**. The z-matrix must be a **machine-emitted artifact**
   (add a z-score emission step to `--matrix`), not hand-derived in the draft prose.
2. **Per-TRIGGER own-force isolation still FAILS for F and V.** On the F-trigger T(38.25) > F(24.00);
   on the V-trigger T(40.80) dominates, V=0. The v1 "приговор" (a trigger must excite its own force)
   is unfixed per-trigger; the per-force z-score passes only by collapsing across triggers. **Disclose
   this explicitly** — report both the per-trigger raw matrix AND the per-force z-gate, and state which
   forces pass which. Do NOT pre-assert "5 forces isolate."
3. **Only 5 of 7 forces carry signal.** V and S columns are all-zero (matrix + live term_V=0). Frame as
   a **5-force** result with V/S honestly inactive — not "7-force isolation."
4. **Orthogonality is incomplete.** Only corr among A/H/F/V was computed; **B and T (largest magnitudes,
   highest collinearity risk) were excluded.** Compute ALL pairs incl corr(B,*), corr(T,*), corr(A,T),
   with corpus N + a permutation/bootstrap CI (the exact 0.000 values look degenerate, not measured).
5. **Long-run contradiction.** T:trauma owns 1975/2000 turns — at trajectory level ONE force dominates
   98.75%. Reconcile with "no single force dominates": the z-gate disproves per-trigger dominance, while
   raw long-run argmax shows trauma monopoly by scale (the L1 artifact reappearing, now trauma). The
   paper must state both, not hide the second.
6. **Statistics absent.** n=40 single run, no seeds/variance/CI. Add: McNemar/sign-test on the 29/40→0/40
   flip; Wilcoxon signed-rank + CI + effect size on resonance/entropy/emergence/debt deltas; per-cell
   mean±sd over N=5 on the matrix. Clarify determinism: if the seed is fixed, "N=5 repeats" measures
   reproducibility, not variance — inject seed variation for real dispersion or drop the N=5 framing.

## Pod / budget / discipline (CPU/GPU split — money fix)

- **CPU phases (P0 build/test, P1-P6, P8, P10, P11 C/Go, P12) run on polygon ($0)** — they use
  `dario`/`kk`/`sartre`/`infer_v4` CPU harnesses; the A100 would idle ~5h through them.
- **A100-SXM4-80GB SECURE only for P7 (540-cell sweep) + P9 (cross-arch duet) voice inference.**
  Target **≤2-3 GPU-hours ≈ $3-5**, hard kill at 3 GPU-h. Bring the pod up only when CPU phases done.
  ssh via two-hop polygon, key `~/.ssh/id_ed25519_polygon`, save pod id LOCALLY. Stop+delete at end.
- Volume ≥ 20 GB (weights `ataeff/dario` ≈ 3.4 GB + sqlite + logs + sweep transcripts).
- **Tools FIRST** (lesson: missing `jq`/`column` silently broke harnesses): `build-essential git
  libsqlite3-dev jq bsdmainutils sqlite3` + `hf` CLI in venv (Python for data prep / weight download
  ONLY, never inference).
- Singularity: reproduce → ONE hypothesis → minimal change → re-run, ≤3 tries/bug, log each. No scope
  creep (a sweep fail ≠ patch the equation). Codex/Opus review pre (this file) + post (the numbers).
- Provenance: every paper-v2 number ↔ an artifact path under `runpod/2026-06-02_full/<NN_phase>/`.
  No recall, no "as before". Keep `master.log` (command order + seeds) and scp it back — it was missing
  from the partial run.
- **pipefail (codex #7 — false-PASS risk):** every piped command (build / `make test` / harness through
  `tee`) runs under `set -o pipefail` and checks `${PIPESTATUS[0]}`. No bare `cmd | tee` may gate a PASS;
  a tool that exits nonzero behind a pipe must fail the phase, not be masked by `tee`'s exit 0.

## Phases (full protocol — none dropped)

**P0 — Pre-flight.** FIRST capture `git status --porcelain` to /tmp BEFORE any `mkdir runpod/...`
(else the cleanliness gate self-falsifies). Six build configs (`dario`, `sartre`, `kk`, `full`, `all`,
manual `dario_kk_only` SOFT — flag-coupling, don't block); then **`make infer_v4` LAST**, before P0.5's
baseline copy. Toolchain (clone+install notorch + ariannamethod.ai if `libnotorch.a`/`libaml.a`/`amlc`
absent). `make weights` (hf download ataeff/dario: janus_v4_base_22k, _sft_leo/_arianna/_yent,
resonance_200m_lora_yent, leo_janus_d12_f16, tokenizer.pkl, tokenizer_yent) → sha256 log + size ±10% of
3.4 GB. `make test` → **capture actual run/passed/failed from the log; gate = failed==0** (1780 is a
finding, not an asserted constant). Pin git HEAD.

**P0.5 — infer_v4 CLI extensions.** `cp infer_v4 infer_v4_v1_baseline` first. Add `--rep-penalty F` +
`--chat-tokens` (BOS/USER/ASST special-token wrapping). Byte-equality regression vs baseline when flags
default. After patch: `make clean && make all && make infer_v4` (note clean wipes `bin/` + `aml/*.c`).

**P0.6 — AML / Go CLI surface.** `mkdir -p bin && make aml-bins && make go-bins` BEFORE any `--help`
capture (clean wiped them). Capture `--help` for `aml/dario_{infer,dialogue,forum}` +
`bin/dario-{infer,dialogue,forum}`; commit `flag_matrix.tsv`. Drop explore (Python-only); duet/trialogue
→ Go binary only; no `--kk-db` on Go.

**P1 — Equation correctness (7 forces, `make dario` alone) — THE CORE OF v2.**
`./dario --matrix`: per-trigger raw matrix + BOTH controls + token-delta dump + orthogonality (ALL pairs
incl B,T). **N=5 seed protocol (codex #4):** the matrix harness reseeds deterministically per cell, so
5 bare repeats are byte-identical — that measures *reproducibility, not variance*. Either (a) drive the
seed override with a fixed schedule `seed ∈ {1,2,3,4,5}` per cell and report per-cell mean±sd across
those seeds, or (b) if no seed-vary hook exists, relabel as **N=1 deterministic + a reproducibility
repeat** and DROP all variance/sd language. Decide in P1 setup and state which. **Emit the z-scored
matrix as a machine artifact.**
Report per the RESULT-1 HONESTY block: raw labeled raw; per-trigger failures for F/V disclosed; per-force
z-gate result; the count the **full 4-gate + tie-rule** (REBUILD_PREREG.md:16-22,69 — specificity 1.5×,
within-trigger argmax, causation vs null, mechanism-not-gain, |top−2nd|<0.1×top ⇒ no isolation) actually
yields (don't pre-assert 5). **Head-to-head vs legacy `bdacb6a`** on identical neutral prompts:
distributional (raw-argmax dominance, NOT token-exact — no `--seed` for dario generation) + **McNemar
p-value** on the 29/40→0/40 flip. **Ablation arm (codex #6 — pin exact builds):** A0=`bdacb6a` (neither),
A1=provenance-only (cherry-pick the `g_input_*` input-accumulator commits WITHOUT the orthogonal-feature
force rewrite — exact commit range pinned in P1 setup before running), A2=`118fa98` (both); same 40
prompts for all three. **If A1 cannot be cleanly built/isolated, DROP the ablation and attribute the
reversal to the combined rebuild — do not decompose a cause you did not measure.** Token-delta status per force (B/A direct →two/→echo; H/F/T column
only — criterion-4 unmet for 3/5, state it). Also emit dest_magnitude vs argmax-rate side-by-side for
legacy (shows destiny "wins" by L1 density, not magnitude — the artifact made explicit).

**P2 — Emotional chambers (6 + Kuramoto K=0.02).** Each chamber activates/decays/drives somatic markers/
synchronizes. Instrument `/stats` if it doesn't expose chambers. **Check the R2 sign:** the partial run
showed trauma ANTI-tracking injected dissonance (falls as dissonance rises) and diss_mean collapsing to 0
at 100% — find whether it's a sign error / mislabel / real, and report honestly. Note kuramoto coupling
looked nearly inert (cluster label barely moved means) — explain or report as weakened, not "held".

**P3 — Velocity operators.** Priority order; dissonance→velocity histogram. Expect T:trauma to lead
post-rebuild by raw scale — report as **changed-from-v1**, not failure.

**P4 — Seasons + laws of nature.** Cite the **2000-turn** run (the 500-turn one never completed a cycle —
degenerate). β-spring/α-summer drift holds (clock-driven). **Reconcile:** same run has T:trauma 1975/2000,
seasonal forces dominate 0 turns — report the automaton-held AND the force-dominance-changed together.

**P5 — SARTRE kernel (R5) — was ABSENT, must run.** Standalone `./sartre_kernel` (RAM/tongue-tier
routing, ringbuffer, OverlayFS base/delta) + `dario+sartre` JSON introspection; capture the SARTRE state
dump on THIS host (v1 numbers were host-specific). Build+run `test_slot_caps.c` (caps 16/8/32/8) and
`register_models.c` (doc MAX_MODELS=4 cap).

**P6 — KK Knowledge Kernel (R6) — INJECTION.** `make all`. Ingest `docs/`; verify sentence-boundary
injection; debt/destiny-drift/trauma-accrual in SQLite; topical-vs-unrelated debt directionality. **KK
fulfillment was 0 in 100% of partial-run records** — run long enough / construct input to drive **nonzero
fulfillment**, else the KK-scoring-matches-spec claim cannot be re-asserted (report which).

**P7 — Voices / sampling sweep (R7) — GPU, 540 cells — was ABSENT, must run.** leo/arianna/yent/leo24m ×
6 temp × 2 top_k × 3 rep_pen × 3 prompts (≥3/10 markers PASS). Champions (leo 0.7/∞/1.3, arianna 0.8/40/
1.4, yent 0.9/40/1.3, leo24m 1.0/40/1.3) are the **v1 hypothesis under test** — PASS = sweep re-derives
them OR documents drift; neither pre-assumed. **Unaffected by the equation rebuild** (uses Janus/Resonance
weights) → expected to match v1; if it doesn't, that's a separate finding.

**P8 — Modes: chain / duet / trialogue (CONVERSATIONS BETWEEN MODELS) + forum.** `bin/dario-dialogue`
chain (self-continuation), **duet** (two voices), **trialogue** (three voices) + KK + field;
`dario-forum` HTTP multi-agent. Capture full transcripts. Core model-to-model surface, omitted from the
partial run. Weights-driven → expected v1-consistent.

**P9 — Cross-arch duet (R-cross) — was DROPPED, re-inserted.** `bin/dario-dialogue --mode duet --voice
yent --voice2 resonance-yent` ≥10 turns; both Janus and Resonance backends load and converse.

**P10 — Web UI.** `./dario --web` smoke; `/api/chat` JSON contract (resonance/entropy/emergence/dominant).

**P11 — Parity (AML/Go vs C).** Token-exact ONLY for the seed-plumbed `infer_v4`(C)/Go path; the
`dario`-routed and AML cells (no `--seed`) drop to lexical/distributional comparison — state this.

**P12 — Coherence / unblockers.** Fixed-prompt generation **rebuilt vs legacy `bdacb6a` — run BOTH**
(legacy generation transcript was never captured; "as legacy would produce" was an untested counterfactual
— drop it or measure it). **Pre-registered degradation rule (codex #5 — fixed before looking):** the
rebuild FAILS the coherence gate iff ANY of — (i) Wilcoxon signed-rank on paired per-prompt resonance
gives p<0.05 AND median Δresonance < −0.15 (i.e. >~20% drop); (ii) entropy median drops >30%;
(iii) emergence median drops >30%; (iv) any prompt yields degenerate output (empty / single-token-repeat
/ NaN field metrics). Debt rising is NOT a failure (F now tracks honest violations). The in-hand deltas
(resonance −9%, entropy −4%, emergence −5%) PASS this rule; the rule is fixed here so the verdict cannot
move after seeing results. Disclose that REPL generation is largely template/canned text keyed to the dominant-force label, not
free prose — so coherence rests on field metrics, with that caveat stated.

**P13 — Doc + archive + post-review.** Everything to `runpod/2026-06-02_full/<NN_phase>/` incl master.log.
Codex POST-audit of the numbers (overclaim hunt). Then write edition 2 strictly from these artifacts.

## Acceptance for the run as a whole
Result-1 metric = frozen z-score, **machine-emitted** (raw labeled raw, z beside it); per-trigger F/V
failure disclosed; **5-force** framing (V/S inactive); ALL-pairs corr incl B/T with CI; head-to-head with
McNemar p; matrix N=5 mean±sd (determinism clarified); coherence with pre-registered threshold + Wilcoxon.
Both controls, full 4-gate+tie-rule, synthetic-trigger + vocab=380 scope disclosed (the promised
"full-scale" validation does NOT exist yet — say so), no gain-tuning. CPU/GPU split honored (≤3 GPU-h).
Held/changed reported honestly per phase, no silent omission. v1 NOT deleted; legacy `bdacb6a` frozen; v2
marked "Second Edition — corrected". Codex/Opus review PRE + POST PASS.

## Audit fixes applied (trace)
A1 infer_v4-last + git-clean-first (P0) · A2 aml/go-bins before help-loop (P0.6) · A4 git-clean capture ·
B5 Phase 9 re-inserted · B6 SARTRE slot-cap/register harnesses (P5) · B8 make-test actual-count gate ·
C9 raw-vs-zscore "5 forces" → report what 4-gate yields, z machine-emitted · C10 full 4-gate+tie-rule ·
C12 champions = hypothesis · D14/D16 T-trauma raw-scale disclosure (P2/P3/P4/P6) · E17/E18 CPU/GPU split
(money) · F19 dario non-determinism → distributional head-to-head, parity token-exact only infer_v4/Go ·
F20 HEAD=118fa98 · plus findings-doc overclaims (raw argmax, per-trigger F/V failure, 5-not-7, corr B/T,
n=40 stats, T-trauma long-run, KK fulfillment=0, R2 trauma anti-track, Phase-5 template, byte-identical
provenance, build parity).
