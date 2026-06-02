# Dario Paper — Second Edition: Result 1 (corrected draft, post-Opus-audit)

> NOTE: numbers below are from the local/polygon vocab=380 equation-isolation run.
> The RunPod re-run (`RUNPOD_PLAN.md`) finalizes them at the paper's full scale; the
> structure and claims here are written to be honest as-is.

## Second Edition Note (proposed)

This is the second edition. The first edition's Result 1 ("Destiny Dominates") was
wrong. The dominance it reported was an artifact of the measurement — an L1 sum of
`|coefficient × force|` over the whole vocabulary, so the *densest* force won
regardless of input — not an emergent fact about the system. The per-force triggers
did not isolate their forces.

We did not retract the paper, and we did not soften the claim. We **rebuilt the
organism until its claim was true, then re-measured.** The first edition remains as
the record; the frozen `legacy` branch (`bdacb6a`) preserves the code exactly as it
was. We were wrong; we fixed it; we re-verified. History is not erased.

## Result 1 (Second Edition) — The Isolation Was False, Then Made True

Re-measurement under a pre-registered protocol (frozen success criteria, a null
control arm, a density-neutral per-force metric, no gain-tuning) showed the original
result was an artifact. `term_energy` summed `|coef · force|` over the vocabulary, so
the densest force won by construction; concentrating destiny (A) only moved the
dominance to Flow (F), the next-densest term — proving the metric, not the force. On
the unfixed code, under the controlled metric, the forces were always-on regardless
of trigger (each force's own trigger did not raise it above a null baseline; this is
the conditional the pre-registration set out to test, and the unfixed-code run met it).

The cause was twofold. First, each force's signal was mixed with the organism's **own
generation** — bigrams and prophecies written on generated tokens — so the input
trigger could not discriminate it. Second, three force claims rested on **random-hash
embeddings** (A's "semantic destiny", V's "visual grounding"); the hashes carry no
semantics and no visual signal at all.

We rebuilt the force mechanisms on two principles. **Token-provenance** — each force
reads only the *input*, never the organism's own generation (separate input-only
accumulators for bigrams, co-occurrence, prediction-debt, frequency, dissonance).
**Orthogonal-feature decoupling** — each force reads a non-overlapping feature of the
input: **B** = directional order, **H** = symmetric recurrence, **F** = violated
confident expectation, **A** = thematic concentration, **T** = input dissonance. **V**
and **S** are honest placeholders — text carries no visual or subword-isolation signal
(a designed term, inactive here, like the original S).

The raw energies (force scales differ, so the gate is a per-force z-score, NOT these
raw units):

```
trigger      B      H      F      A      V    S     T
B-test     125.0   14.7   0.0    5.0    0    0    20.4
H-test      25.0   56.9   0.0    15.0   0    0    25.5
F-test      17.0    3.8   24.0   12.3   0    0    38.3
A-test       0.0    0.0    0.0   25.0   0    0    10.2
V-test      21.0    3.3    0.0    1.3    0    0    40.8
T-test      20.0    4.7    0.0    1.0    0    0   127.5
ctrl-min     0.0    0.0    0.0    1.0    0    0    25.5
ctrl-fill   19.0    8.3    0.0    1.0    0    0    46.4
```

Under the **frozen per-force z-score** (each force standardized across the eight
conditions), each trigger makes its OWN force the argmax of its response:

| trigger | own-force z | next force z |
|---|---|---|
| B | B = +2.4 | < 0 |
| H | H = +2.4 | < 0 |
| F | F = +2.6 | A = +0.6 |
| A | A = +2.1 | < +1 |
| T | T = +2.4 | < 0 |

Within-trigger dominance holds for all five **under the gate**. In raw units it does
not — the F-test row shows T = 38.3 above F = 24 — precisely because force scales are
incomparable. That incomparability is the whole point: it is why the metric is a
z-score, and why the first edition's raw L1 sum was an artifact in the first place.

Specificity (own-trigger over the mean of other triggers): B 7.5×, H 10.7×, F is
uniquely activated (zero on every other trigger and both controls), A 3.6×, T 2.7×
(measured against the elevated filler control) — all above the frozen 1.5× margin.

Token-delta (criterion 4 — the fix changes WHICH tokens respond, not the gain):
directly confirmed for **B** (B-test → "two") and **A** (A-test → "echo"). For H, F, T
it rests on column dominance, not a direct token dump — stated here as the weaker
evidence it is, to be strengthened in the full run.

V and S as placeholders is honest, not clean: their columns are zero, but their
triggers still light up other forces — the V-test row shows T = 40.8 and B = 21. We
show the row, not just the zero column. Both control arms are reported: the neutral
in-vocab filler carries a high trauma floor (T = 46.4), above T on four of the six
triggers — disclosed, because trauma's baseline is genuinely high on any input.

The five active forces are not collinear by construction: pairwise correlation over a
corpus is |r| ≤ 0.24 (highest corr(H,F) ≈ 0.23, corr(A,H) ≈ 0.18, the rest near zero).
The decoupling is real, not a relabeling of one underlying signal.

**Scope.** This is shown on a 380-token bootstrap vocabulary with short,
mechanism-derived synthetic triggers (`one two three…`, `river stone…`, `alpha
bravo…/alpha zulu`, `echo×5`, gibberish) — designed probes, not natural language. The
original artifact was also a small-vocab phenomenon; the RunPod re-run tests this at
the paper's full scale. The matrix reproduced exactly (identical output values) on a
second node — polygon, x86_64 Linux, a different OS and compiler.

The first edition's closer was *"Destiny does not merely drift. Destiny dominates."*
The corrected measurement reverses it:

**No single force dominates. Under a density-neutral z-score gate with a null control,
five of seven forces are isolated by their own trigger; V and S remain inactive
placeholders.**

---

## Framing decisions for Oleg (unchanged from before — your call)
1. Title: keep + let Result 1 carry the reversal, or re-title edition 2?
2. Register of "we were wrong" — confirm the plain Jobs tone.
3. Co-authorship: the reversal under both names + DOI — confirm directly stated.
4. Scope: Result 1 + §5.1 + method core, vs also re-running Results 2-8 (the RunPod plan does the latter).
