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

## Current state (after f993f2f)
- H: 3/4 gates (need token-delta + coherence). CLOSEST.
- B: noisy asymmetry, not column-leading cleanly. NEEDS WORK.
- F: flood killed; trigger indirect. WIP.
- A / V / T: not addressed.
- token-delta + coherence gates: not yet run.
- Triggers in harness: EXPERIMENTAL, not frozen in pre-reg.

## Next
1. Finish H: token-delta gate + coherence (generation intact). Close first force fully.
2. Strengthen B (asymmetry too weak).
3. A / V / T same recipe (orthogonal-feature mechanism + derived trigger).
4. Full matrix 4-gate simultaneous + coherence.
5. E4: re-run on polygon (x86) / runpod, then Dario paper 2 (corrected numbers,
   "prior was false" AFTER the fix, §5.1 SARTRE/KK honesty). legacy = "what was".
