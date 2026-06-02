# Dario Paper 2 — Comprehensive RunPod findings (2026-06-02, A100)

> ⚠ **SUPERSEDED / INTERIM — partial-run evidence, audited and found to overclaim.** This doc is the
> PARTIAL pass (Results 1-8 + coherence + laws), kept for provenance. An Opus + Codex adversarial audit
> (2026-06-02) flagged real вши that the **full run (`RUNPOD_PLAN_V2_FULL.md`) must correct before any
> number here enters paper v2**:
> 1. The live head-to-head dominance (legacy A:destiny 29/40 → rebuilt 0/40) is **RAW-argmax**, NOT the
>    z-score gate. Real and verified by recompute, but it shows "which force has the biggest unnormalized
>    scale changed identity" — T's raw scale grows unbounded. State RAW + report the z-scored version.
> 2. **Per-TRIGGER own-force isolation still FAILS for F and V** (F-trig: T 38.25 > F 24; V-trig: T 40.8,
>    V 0). The v1 "приговор" is unfixed per-trigger; the per-force z-gate passes only by collapsing across
>    triggers. Disclose, don't hide.
> 3. **5 of 7 forces active** (V, S all-zero) — not "7-force isolation". 4. Orthogonality computed only
>    among A/H/F/V; **B and T excluded** — compute all pairs + CI. 5. **n=40 single run, no seeds/variance/
>    CI** — add McNemar + Wilcoxon. 6. **T:trauma owns 1975/2000 long-run** — contradicts "no single force
>    dominates"; reconcile (raw-scale artifact reappearing as trauma). 7. The z-scores in the result-1
>    draft were **hand-derived in prose**, no machine artifact. 8. **Phase-5 generation is canned template
>    text** (C-source comments + fixed word list), not free prose; "register preserved vs legacy" has **no
>    legacy transcript** — counterfactual, must be measured or cut. 9. "byte-identical Intel+polygon" was
>    verified earlier (REBUILD_LOG E2-E4) but those artifacts are not in this run dir — cite the source.
> 10. R2 trauma **anti-tracks** injected dissonance (falls as it rises) — sign issue, report honestly.

Source artifacts: `runpod/2026-06-02-comprehensive/logs/` (matrix, 8 harness jsonl,
coh_rebuilt.jsonl, coh_legacy.jsonl, 04_laws_2000.jsonl, generation, master.log).
Run: A100-SXM4-80GB SECURE. Binary cloned at HEAD `78d101f`; current repo HEAD `118fa98` is code-identical
(it only added this doc + the log entry), so the binary == `118fa98` code. Legacy HEAD `bdacb6a`.

## Finding A — the reversal measured head-to-head (the gem)

Identical 40 neutral held-out prompts, same `/api/chat` path, two binaries:

| metric | legacy (bdacb6a) | rebuilt (78d101f) |
|---|---|---|
| dominant-force histogram | **A:destiny 29/40**, F:prophecy 11/40 | **B:chain 27/40**, T:trauma 13/40, **A:destiny 0/40** |
| resonance (mean) | 0.760 | 0.690 |
| entropy (mean) | 0.488 | 0.470 |
| emergence (mean) | 0.396 | 0.375 |
| debt (mean) | 2.10 | 4.69 |

The legacy code **reproduces "Destiny Dominates"** — A:destiny is the per-turn argmax on
29 of 40 neutral prompts. The rebuilt code, on the same prompts, makes destiny the argmax
**zero** times; B:chain leads. The dominance vanishes precisely when forces read honest
input-provenance features instead of dense random-embedding cosine spreads. The reversal is
not confined to the synthetic isolation matrix — it is demonstrated in live generation on
neutral input, head-to-head against the frozen original.

## Finding B — R4 (laws of nature) survives the rebuild at full 2000-turn scale

Full 4-season cycle (501 summer / 501 autumn / 499 winter / 499 spring):

| season | alpha | beta | gamma | tau |
|---|---|---|---|---|
| spring | 0.289 | **0.311** | 0.320 | 1.050 |
| summer | **0.352** | 0.169 | 0.284 | 0.914 |
| autumn | 0.301 | 0.150 | 0.292 | 0.838 |
| winter | 0.299 | 0.153 | 0.311 | 0.843 |

β peaks in spring, α peaks in summer, τ drifts 1.05→0.84 — the documented seasonal automaton
holds. The seasonal-drift Result is independent of the force-mechanism rebuild. (Long-run
dominant histogram: T:trauma 1975/2000 — trauma accrues, T leads by raw scale over time.)

## Finding C — coherence preserved, not collapsed (Phase 3 anti-degradation gate)

resonance −9% (0.760→0.690), entropy −4%, emergence −5%; debt +123% (2.10→4.69, F now tracks
real violated input expectations). Generation register preserved (Phase 5: same code-comment +
word-association output as legacy would produce). A matrix gain that degraded text to gibberish
= FAIL; this is a modest, explainable shift, not degradation.

## Finding D — V honestly inactive in matrix AND live

test_08: term_V_mean=0, vis_mean=0 across all 30 turns. Matrix V column = 0. The placeholder
claim is confirmed on a real run, not asserted.

## Finding E — Results 2-8 secondary (held/changed)

- R2 dissonance ramp: trauma/fear track input; dominant B:chain. Held.
- R3 velocity: 49/50 STOP on repeated input; dominant T:trauma 43/50. Held.
- R5 kuramoto: LOVE/RAGE/VOID all T:trauma-led (12-13/cluster), B:chain second. Held.
- R6 swiglu gate: H rises with resonance priming (1.4→7.0 across levels 0→80), F modest — gate works.
- R7 prophecy debt: 184/200 T:trauma; velocity WALK/UP dominant. Held.
- KK (R6/test_04): directional — unrelated input → more prophecy debt (30.6) than topical (18.8);
  fulfil_mean=0 on short runs.

## Matrix (Result 1) — reproduced on A100, byte-identical to Intel + polygon

```
trigger  B      H      F      A     V  S  T
B       125.00 14.73  0.00   5.00  0  0  20.40
H        25.00 56.90  0.00  15.00  0  0  25.50
F        17.00  3.77 24.00  12.29  0  0  38.25
A         0.00  0.00  0.00  25.00  0  0  10.20
V        21.00  3.33  0.00   1.27  0  0  40.80
T        20.00  3.02  0.00   1.00  0  0 162.44
CTRL_min  0.00  0.00  0.00   1.00  0  0  25.50
CTRL_fill19.00  8.32  0.00   1.00  0  0  46.41
corr(A,H)=0.236  all other pairwise corr = 0.000
token-delta: B-trig→two(25), A-trig→echo(25), T-trig→resonance/field/destiny(6 each);
             F-trig & H-trig top-token flat (column-dominance evidence, not direct token dump)
```
