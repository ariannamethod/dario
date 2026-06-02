# Dario Rebuild — Live Fix Log

Append-only chronology so a context summary never loses the thread.
Goal: each of the 7 forces excited only by its own trigger (diagonal dominates,
4-gate honest), code raised to the claim — no metric-gaming, no retreat.
Branch: `main` (`~/q/dario`, origin ariannamethod). Backup: frozen `legacy` = bdacb6a.
Spec: `REBUILD_PREREG.md` (4-gate, density-neutral metric, control arms, no gain-tuning).
Harness: `./dario --matrix` (gated `g_matrix_mode`, deterministic reset/cell, raw
pre-renorm energy `g_raw_energy`, snapshot `g_snap`, 6 triggers + 2 control + orthogonality).

## 4-gate (per force X)
1. specificity: act_X(trig_X) > act_X(trig_Y) ∀Y, margin ≥1.5×
2. within-argmax: X is the argmax force inside trig_X's own response
3. causation: act_X(trig_X) > baseline_X (control), margin ≥1.5×
4. token-delta: fix changes WHICH tokens respond, not just gain

---

## Chronology

**E0.5 — harness + reachability** (commits 92e59ec, cb7e123, 8ba12fd)
- Built `--matrix` harness; measurement-only, normal generation untouched.
- Reproducibility gate: found+fixed `rng_state=time(NULL)` (dario.c:1771) → deterministic
  per-cell seed. Two processes → identical matrix. PASS.
- CTRL fix: CTRL_empty read stale energy (no compute) → CTRL_minimal feeds "the".
- Orthogonality: corr(A/H/F/V) all |r|<0.20 (max 0.184) << 0.70 → **forces separable,
  goal reachable.** Audit's non-separability fear refuted.
- Baseline (unfixed): no force passes causation; forces are always-on-on-presence.

**E1 — root causes** (verified, file:line)
- B=`bigram_row(last)` (1275): last-token only · H=`Σcooc·dist` (1293): cooc richness ·
  F=prophecy_add every gen-token (1645): always-on-on-content · A=destiny EMA (1331):
  fills from any word · V=visual (1352): no visual signal in text · T=trauma gate (1341):
  seasonal-artifact, not trigger.

**E2 — decouple by orthogonal input features** (commits 007bf0b, f993f2f)
- **B**: last-token bigram → directional asymmetry `count(a→b)−count(b→a)` over window
  (1283). Responds to STRICT ORDER; symmetric recurrence cancels (=H's signal).
  B↔H decoupled at column level. STATUS: weak/noisy (B~3–4), does not cleanly lead its
  column yet — needs strengthening.
- **F**: prophecy added every gen-token (0.3) → flooded 300–440 everywhere, broke
  within-argmax for ALL. Now registers only on CONFIDENT prediction (cooc≥2, 1645).
  **Flood killed: 300–440 → 0–28.** STATUS: trigger is INDIRECT (driven by generation
  prediction dynamics, not input pattern) — re-derivation WIP.
- **H**: unchanged mechanism (symmetric cooc). With F-flood gone, H now passes 3/4 gates:
  H-trig H=133 = column max, 2.6× 2nd, 6.3× baseline(21), within-argmax ✓. CLOSEST to done.

**E2-H — H ISOLATED, first force done** (commit e977c58)
- Token-delta gate added to harness. Coherence verified PASS (generation still
  thematic/coherent after B/F changes; B/F are minor logit terms).
- H passes 4/4: specificity (133=col max, 2.6×), within-argmax, causation (6.3×
  baseline), token-delta (active set differs: measurement/observation vs
  saturation/hysteresis). Residual: H slightly elevated everywhere, margin clean.

**E2-B squared + generation-coupling diagnosis** (commits d26ca47)
- Squared the asymmetry (amplify strict order). B rose but still does NOT lead its
  column (B-trig 6.09 vs H-trig 6.33).
- ROOT DIAGNOSIS (precise): B is polluted by the organism's OWN generation. During
  process_input the generated tokens are appended to context (dario.c:1680) and
  `bigram_update(last, next, 0.5)` writes generation transitions (dario.c:1651).
  So the bigram table = input + generated, generation dominating. The input trigger
  cannot cleanly discriminate B. SAME class as F (generation-driven).

## Current state (after d26ca47)
- **H: DONE 4/4.** ✅ First force isolated, coherence intact.
- **B / F: generation-coupled** — their signal is mixed with the organism's own
  generation (bigrams 1651, context 1680; prophecy 1645). Input triggers don't
  cleanly discriminate them. FIX (deep): token-provenance — track input→input
  transitions separately from generated ones; B/F read only the input view.
- A / V / T: not addressed.
- Triggers in harness: EXPERIMENTAL, not frozen (freeze at E3).
- Coherence: provisional PASS (smoke); rigorous before/after-vs-legacy diff at E3.

**E2-B — B ISOLATED via token-provenance** (commit ccf1c61) ✅ SECOND FORCE
- Added `g_input_bigrams`: input-only bigram table (written from input transitions in
  context-update, cleared per cell). B reads directional asymmetry directly from it,
  independent of the context window (whose tail is generated tokens).
- Result: B-trig B=125 = col max, 5× 2nd (25), baseline 0.00, within-argmax (125>H46),
  token-delta trivial. **4/4.** B↔H cleanly decoupled. Provenance method VALIDATED.

**E2-F — F ISOLATED via provenance** (commit bbc2649) ✅ THIRD FORCE
- `g_input_debt`: debt of CONFIDENT input predictions the input itself violated
  (confident a→b, then a→c ⇒ debt[b]). F reads it; generation-prophecy kept for gen.
- F-trig ('alpha bravo×4 then alpha zulu') F=24, ZERO on every other trigger + baseline.
  Raw H=25.66 marginally over F=24, but under FROZEN per-force z-score metric F z=+2.2
  dominates (V+0.95 A+0.68 H−0.7 B−0.5). 4/4 under frozen metric. Provenance validated x2.

## State (after bbc2649): H ✅, B ✅, F ✅ — THREE forces isolated
- A (destiny EMA): always-on. Target = semantic convergence (coherent input → strong
  destiny magnitude → high A). NEEDS: meaningful embeddings (check get_embed source).
- V (visual): no visual signal in text — likely honest-inactive (S-like), or feed visual.
- T (trauma): dissonance/alien gate (seasonal-artifact at 1341, 1220). 
- Triggers: experimental, freeze at E3. Coherence: provisional PASS (recheck after A/T).

## NEXT
A: respond to semantic coherence via destiny magnitude (if embeddings meaningful).
Then V (honest status), T (dissonance gate). Then full matrix 4-gate simultaneous +
coherence diff vs legacy + freeze triggers in pre-reg + E4 (polygon/runpod + paper2).

## Next
1. Finish H: token-delta gate + coherence (generation intact). Close first force fully.
2. Strengthen B (asymmetry too weak).
3. A / V / T same recipe (orthogonal-feature mechanism + derived trigger).
4. Full matrix 4-gate simultaneous + coherence.
5. E4: re-run on polygon (x86) / runpod, then Dario paper 2 (corrected numbers,
   "prior was false" AFTER the fix, §5.1 SARTRE/KK honesty). legacy = "what was".
