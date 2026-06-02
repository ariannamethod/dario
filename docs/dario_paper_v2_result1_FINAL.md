# Result 1 (Second Edition, FINAL) — The Isolation Was False, Then Made Partly True

> Audited (Opus + Codex + adversarial recompute), machine-emitted, statistics-backed. Supersedes
> `dario_paper_v2_result1_draft.md` and the SUPERSEDED partial findings. Artifacts:
> `runpod/2026-06-02_full/01_equation/` (01_matrix.txt = raw±sd + z-gate + per-trigger + corr;
> 01_headtohead_stats.txt). Rebuilt binary at `c77c6b2` (dario.c code; harness now emits the z-gate
> as a machine artifact). Legacy frozen at `bdacb6a`.

## Second-edition note

The first edition's Result 1 ("Destiny Dominates") was an artifact: `term_energy` summed `|coef·force|`
over the whole vocabulary, so the *densest* force won regardless of input, and the per-force triggers did
not isolate their forces. We did not retract or soften — we rebuilt the force mechanisms
(token-provenance: each force reads input-only accumulators, never the organism's own generation;
orthogonal-feature decoupling: B=directional order, H=symmetric recurrence, F=violated confident
expectation, A=thematic concentration, T=input dissonance) and re-measured under a pre-registered gate.
v1 is not deleted; `bdacb6a` is frozen. We were wrong; we fixed it; we re-verified — and we report what
the fix did and did NOT achieve.

## The reversal — measured head-to-head, with statistics

Identical 40 neutral held-out prompts, same `/api/chat` path, two binaries:

| | legacy `bdacb6a` | rebuilt |
|---|---|---|
| dominant force (per-turn raw argmax) | **A:destiny 29/40**, F:prophecy 11/40 | **B:chain 27/40**, T:trauma 13/40, **A:destiny 0/40** |
| resonance mean | 0.760 | 0.690 |

**McNemar on the A:destiny-dominant flip: b=29, c=0, exact two-sided p ≈ 3.7×10⁻⁹.** Every prompt that
showed destiny-dominance in the legacy code lost it in the rebuilt code; none reversed. The "Destiny
Dominates" effect is **real in the frozen code and vanishes** when forces read honest input-provenance
features. This is RAW-argmax dominance (T's raw scale is unbounded) — see the gate below for the
standardized result.

## The isolation gate — machine-emitted z-score, honest about what passes

Per-force z across the 8 conditions (the gate; raw matrix with mean±sd over N=5 seeds is in the artifact):

| force | z-gate | per-TRIGGER raw argmax |
|---|---|---|
| B | **PASS** (own +2.57, best-other −0.09) | B-trig→B ✓ |
| H | **PASS** (+2.56 / +0.19) | H-trig→H ✓ |
| F | **PASS** (+2.65 / −0.38) | **F-trig→T ✗** (T 38.25 > F 24.00) |
| A | **PASS** (+2.07 / +0.88) | A-trig→A ✓ |
| T | **PASS** (+2.57 / 0.00) | T-trig→T ✓ |
| V | **FAIL** (own 0.00 — never activates) | V-trig→T ✗ (V=0) |
| S | placeholder (no trigger) | — |

**Five of seven forces isolate under the z-gate; V and S are inactive placeholders (V's column is all
zero in the matrix AND in live operation — term_V=0 across every test).** We disclose the limit the
v1 "приговор" exposed and the rebuild did NOT fully close: **at the per-trigger raw level, F and V do
not win their own triggers** — T dominates them by raw scale. The z-gate passes F only by standardizing
F's column across conditions (F's own-trigger 24 is huge relative to F's ~0 elsewhere). Both the raw
per-trigger failure and the z-gate pass are reported; neither is hidden.

## Orthogonality — the decoupling is NOT clean (all 6 active pairs, incl B and T)

Pearson r over 380-dim force vectors, ALL active pairs (earlier drafts computed only A/H/F/V, hiding the
two largest-magnitude forces):

```
corr(B,A) = 0.845   ← COLLINEAR (> 0.70)
corr(B,H) = 0.444
corr(H,A) = 0.236
corr(B,F)=corr(B,T)=corr(B,V)=corr(H,F)=corr(H,T)=corr(H,V)
  =corr(F,A)=corr(F,T)=corr(F,V)=corr(A,T)=corr(A,V)=corr(T,V) ≈ 0.000
```

**B (directional chain) and A (thematic concentration) are collinear (r=0.845)** — frequent tokens carry
both directional bigram structure and concentration, so the two features share most of their variance.
B↔H is moderate (0.444). F and T are cleanly orthogonal to everything. The "orthogonal-feature decoupling"
holds for F and T, partially for H, and **fails for the B↔A pair** — stated, not relabeled.

## Coherence — small but SYSTEMATIC drop (not "preserved")

resonance dropped on **38 of 40 prompts** (sign-test normal-approx z ≈ 5.53) — a systematic effect, not
noise. Magnitude is modest: mean 0.760→0.690 (−9%), median Δ≈−0.07, **within the pre-registered
non-degradation threshold (FAIL iff median Δresonance < −0.15)**. So the rebuild PASSES the
non-degradation gate, but the honest statement is "a small, systematic resonance reduction," not
"coherence preserved." Debt rose (2.10→4.69) as F now tracks honest violated expectations — not a failure.

## Ablation — DROPPED per pre-registration

A1 (provenance-only, to isolate which rebuild principle drives the reversal) is **not cleanly buildable**:
token-provenance and orthogonal-feature decoupling were landed together in the same force-block rewrites
(REBUILD_LOG E0.5–E2), not as separable commits. Per the pre-registered rule, we DROP the ablation and
attribute the reversal to the **combined** rebuild — we do not decompose a cause we did not measure.

## Scope (disclosed)

380-token bootstrap vocabulary; short mechanism-derived synthetic triggers (`one two three…`, `river
stone…`, `alpha bravo/alpha zulu`, `echo×5`, gibberish) — designed probes, not natural language. The
matrix reproduced identically on Intel macOS, polygon x86_64 Linux, and RunPod A100 (REBUILD_LOG E2–E7).
**The promised "full-scale" (large-vocab) validation does NOT yet exist** — the matrix is still vocab=380.
N=5 uses a per-rep seed schedule (mean±sd emitted); the bootstrap is otherwise deterministic.

## Honest closer

The first edition said *"Destiny does not merely drift. Destiny dominates."* The corrected, audited
measurement: **No single force dominates by construction. On identical neutral prompts the destiny
monopoly is destroyed (McNemar p≈3.7×10⁻⁹); under a machine-emitted per-force z-gate five of seven forces
isolate (V and S inactive); but the per-trigger isolation still fails for F and V, the B↔A decoupling is
collinear (r=0.845), and coherence drops modestly but systematically. The artifact is overturned; a clean
seven-force orthogonal isolation is not claimed.**
