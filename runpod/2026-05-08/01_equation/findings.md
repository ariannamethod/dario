# Phase 1 — 7 Forces Empirical Test (RunPod 2026-05-08, c4bf242)

## Result: 0/6 expected-dominant tests passed; A always wins under varied input

| Term | Trigger (5 turns) | Expected | Observed | Term energy line |
|---|---|---|---|---|
| B | repeated bigram pair | TERM_B | TERM_A | B:8 H:1 F:9 **A:52** V:9 T:0 |
| H | dense in-vocab cooc | TERM_H | TERM_A | B:8 H:2 F:16 **A:50** V:14 T:0 |
| F | prophecy-driving | TERM_F | TERM_A | B:8 H:1 F:12 **A:45** V:14 T:0 |
| A | destiny drift | TERM_A | TERM_A | B:8 H:1 F:10 **A:51** V:11 T:0 |
| V | perceptual-rich | TERM_V | TERM_A | B:8 H:1 F:22 **A:48** V:15 T:0 |
| T | alien 5 turns | FORCE_TRAUMA | TERM_A | B:8 H:2 F:34 **A:42** V:17 T:0 |
| S | (placeholder) | NEVER dominate | NEVER (S=0, hardcoded `dario.c:1416`) | correct |

## Why A always wins

1. Destiny EMA accumulates fast from prior seed-word turns (`A[i] = cos(embed[i], destiny) * |destiny|`).
2. Alien words become known after 1 exposure (added to vocab; dissonance drops to 0 on next turn).
3. T term spreads logit boost across 50 words; A concentrates per direction.

## Sustained 15-turn unique-alien input (T-trigger, 3x token volume per plan failure recovery)

- d=1.00 across all 15 turns
- velocity UP across all 15 turns; tau ramps 1.30 -> 1.69 (manic acceleration as designed)
- trauma=0.970 (well above 0.3 threshold)
- chambers: fear=0.34, rage=0.31, void=0.23
- debt=5.317 (DOWN velocity would trigger but UP wins on dissonance > 0.8 priority)
- BUT dominant term still A (destiny)

## Paper finding

Dario architecture surfaces destiny dominance across MOST input regimes. README per-term decomposition (each force independently triggerable) is theoretical. Empirically:

- Trauma CHAMBER + VELOCITY + DEBT activate as designed (chamber rises, velocity escalates, debt accumulates).
- Trauma TERM (logit contribution) does not dominate because spread thinly across 50 seed words while A concentrates per single direction.
- Code fragment self-reflection works: A:destiny fragment surfaces when A dominant (verified `dario.c:393-424`).

S correctly contributes 0 in all 7 test runs (not visible in envelope output, hardcoded `dario.c:1416`).

## Per-test artifacts

- `01_equation/per_term/{B,H,F,A,V,T,S}.txt` full envelope captures
- This `findings.md`
