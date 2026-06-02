# Dario Rebuild — Pre-Registration (E0.5)

**Frozen before any fixed-code number exists. Amendments only with logged Oleg approval.**
Written 2026-06-02 against canonical `bdacb6a`. Reason: edition-1 published an
unverified isolation claim; this contract fixes what counts as success *before*
the author can see results, so the metric cannot be reverse-engineered into a
green diagonal. Authored after an independent 3-lens Opus audit (verdict:
revise-plan-first).

## Claim under test (paper Result 1 + §5.1)
"Each of the seven trigger conditions drives a specific force term." The audited
target is the **paper's hard claim** (`docs/dario_paper_*` Result 1 "Destiny
dominates"), NOT the already-softened README:173.

## Falsifiable success criterion
For each force X, isolation holds iff ALL FOUR pass:
1. **Specificity:** `act_X(trigger_X)` > `act_X(trigger_Y)` for all Y≠X, by margin **m** (frozen below).
2. **Within-trigger dominance:** X is the argmax force inside trigger_X's own response (absolute, not just relative).
3. **Causation:** `act_X(trigger_X)` > `baseline_X` (from the null/control arm).
4. **Mechanism, not gain:** the fix changes WHICH tokens respond to the trigger pattern (token-level response delta vs null) — a fix that only rescales a coefficient or global gain FAILS.

Isolation count = forces passing all four. The matrix claim is honest only at the count the run actually yields. No partial credit, no relabeling.

## Metric (frozen)
- Sampled on **pre-renorm, pre-gate** raw force arrays — `B`(1280) `H`(1302 pre `/h_max`) `F`(1319) `A`(1331 pre `/a_max`) `V`(1352) `T`(1345) — NOT post-gate `term_energy` (1403-1417), which inherits the swiglu/vis-enrichment input-independent bias.
- Per force: `ratio_X = act_X(trigger_X) / mean_{Y≠X} act_X(trigger_Y)`, plus z-score of act_X across the 7 conditions (density-neutral). `raw-x100` is REJECTED — still density-biased.
- Per cell: **N repeats with full state reset**, report mean ± spread. A cell is its mean, never a single run.

## Control / null arm (mandatory, run on UNFIXED code first)
- shuffled in-vocab tokens; empty context; scrambled trigger→force labels.
- through the identical pipeline + reset. Defines `baseline_X` per force.
- If even the density-neutral metric shows dense always-on fields on unfixed code, non-separability is reached BEFORE any per-force tuning.

## State reset (per trigger cell — the harness's core)
Reinit: `D` struct, `g_destiny`/`g_dest_magnitude`, `trauma_level`, all 6 chambers,
`cooc`/`bigrams`/`prophecy` tables, `season`/`velocity`. Without this the matrix is
history-coupled (EMA/cooc/prophecy accumulate across triggers) and irreproducible —
this is why edition-1's "F floods every column" appeared.

## Isolation-measurement build (couplings frozen during measurement)
Verified shared nonlinear paths that otherwise re-couple every force:
trauma→γ leak (1377-1378), swiglu gate on H&F (1397-1399), chamber→{α,β,γ}_mod
(1381-1383), V-enrichment of H/F (1364-1367). During measurement: pin
`α_mod=β_mod=γ_mod=τ_mod=1.0`, swiglu gate constant, trauma→γ off, and zero
`g_dest_magnitude` (A-flood OFF) while the other five are measured; fix A's
mechanism last on the clean field. **State explicitly** whether the reported
matrix is the COUPLED organism or the UNCOUPLED equation — they differ.

## Triggers (frozen, re-derived from mechanism not name)
Edition-1 triggers were designed against an imagined mechanism (B=last-token-only,
A=whole-history, T=never-trips). E1 re-derives each trigger from the code's actual
sensitivity (what input maximally moves force X per the real mechanism), then
FREEZES the word-sequences + turn-count here before any fixed-code run.
**BANNED:** the runpod_plan_v3.md:493 "3× token volume on failure" knob. Trigger
strength fixed across all forces and held constant across the whole matrix. If a
force needs more input to appear, that is the FINDING.

## Orthogonality pre-check (decides reachability — run before E2)
Pairwise correlation of A/H/F/V force vectors over a corpus on UNFIXED code.
A, H, F are three cosine projections of the same embedding space (A=EMA 1331,
F=prophecy cosine 1314, H=cooc). **If correlation > 0.70 → non-separable by
construction.** Then the honest outcome is the paper stating "the isolation claim
was false," NOT bending code — escalated to Oleg with numbers, BEFORE spending E2.

## Frozen constants
- margin **m = 1.5×** (ratio_X must exceed 1.5 for specificity).
- correlation escalation threshold **0.70**.
- **N = 5** repeats per cell.
- tie/near-tie rule: |top − 2nd| < 0.1× top ⇒ "no isolation" for that column.

## Coherence gate
After each force fix and after any coefficient rebalance: fixed-seed generation on
N held-out prompts, scored/diffed. A matrix gain that degrades text = FAIL → revert.
`bigram_coeff` rebalance is a logit change with its own before/after generation diff,
not cosmetic.

## Independent re-run binding
A second agent builds from this frozen spec and produces the final matrix
independently, byte-comparing to the author's run. Same author defining
metric+triggers+running+scoring+rewriting = exactly the loop that produced edition-1.

## E0 first gate
Harness validated by: legacy-vs-canonical matrix delta == 0 (same code → same matrix).
