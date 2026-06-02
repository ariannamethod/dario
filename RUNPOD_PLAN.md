# Dario Paper 2 — RunPod Re-Run Plan

Reproduce the paper's experimental frame on the REBUILT code, on RunPod (parity with
the 2026-05-08 v1 run), to get verified numbers for edition 2. Everything on the pod.
Spec: `REBUILD_PREREG.md` (frozen 4-gate, z-score metric, control arms, no gain-tuning).
Discipline: Singularity mode (reproduce→1 hypothesis→minimal change→re-run, max 3),
Codex pre-audit of this plan + post-run audit. scp artifacts before stop;
`volumeInGb=0` ⇒ stop=terminate.

## Phases

**Phase 0 — pre-flight ($0, local/polygon).** Confirm: rebuilt `dario` builds; `--matrix`
reproduces local (B125 H56.9 F24 A25 T127.5); pre-reg triggers frozen; RunPod key + ssh
path verified. Gate before billing a GPU minute.

**Phase 0.5 — on-pod build + regression.** Build `dario`, `dario+sartre`, `dario+sartre+kk`
(the 5 standalone configs). Run `--matrix` on pod → must equal local/polygon (reproducibility).
Save baseline binary.

**Phase 1 — Isolation matrix (the core of edition 2).**
- Run `--matrix`: full diagonal + BOTH control arms (minimal + filler) + token-delta dump
  + orthogonality. N=5 reset/cell.
- Produce the **z-scored** matrix (the actual gate), not just raw — per-force z across the
  7 conditions. Record which forces pass which of the 4 gates: B/H/A clean; F/T under
  z-score; V/S placeholders.
- Artifacts: raw matrix, z-scored matrix, control rows, token-delta, corr(A,H,F,V).

**Phase 2 — Results 2–8 on the REBUILT code (do they still hold after the force rewrite?).**
The rebuild changed force mechanisms — downstream may have shifted. Re-run each, report
held/changed HONESTLY:
- R2 chamber co-activation · R3 velocity priority · R4 laws of nature (2000 turns) ·
  R5 SARTRE introspection · R6 KK scoring · R7 sampling sweep (540 cells, GPU) ·
  R8 chain recovery.

**Phase 3 — Coherence.** Fixed-seed generation on N held-out prompts, diff vs legacy
(bdacb6a). A matrix gain that degraded text = FAIL.

**Phase 4 — Archive + audit.** Capture all to `runpod/2026-06-XX/`. Codex post-audit.
Then draft edition 2 strictly from these numbers.

## CHECKLIST — is the plan (and run) correct?
A claim is admissible to paper 2 ONLY if every box holds.

### Plan correctness (before the pod)
- [ ] Every edition-2 claim maps to a planned RUN + artifact path (no recall, no "as before").
- [ ] Metric is the FROZEN per-force z-score; any raw number in the paper is labeled raw + carries the z-score beside it (the edition-1/draft вошь must not recur).
- [ ] BOTH control arms (minimal + filler) are reported, not the favorable one.
- [ ] V row AND S row shown (placeholders, but their triggers' off-diagonal activation visible).
- [ ] Token-delta status stated per force: B/A direct, H/F/T via column dominance — not implied for all.
- [ ] Orthogonality numbers (corr, |r|≤0.236) included as the anti-collinearity defense.
- [ ] Synthetic-trigger + vocab=380 scope disclosed as limitations.
- [ ] Pre-reg compliance: N=5, full reset/cell, frozen mechanism-derived triggers, no gain-tuning, no 3×-volume knob.
- [ ] Codex PRE-audit of this plan: PASS (data handed to it).

### Run correctness (on the pod)
- [ ] Reproducibility: pod matrix == local == polygon (deterministic).
- [ ] Phase 0.5 regression: 5 standalone builds OK; baseline saved before any patch.
- [ ] Results 2–8 each reported held/changed with its own artifact — no silent omission.
- [ ] Coherence diff vs legacy: PASS (no degradation).
- [ ] Singularity discipline: ≤3 tries per bug, logged; no scope creep (a sweep fail ≠ patch the equation).
- [ ] All artifacts scp'd to local + committed BEFORE pod stop/terminate.
- [ ] Codex POST-audit of the run: PASS.

### Edition-2 correctness (the paper)
- [ ] v1 NOT deleted; legacy (bdacb6a) frozen; v2 marked "Second Edition — corrected".
- [ ] "We were wrong, we rebuilt, we re-verified" stated plainly (Jobs register, no hedge).
- [ ] Abstract lawyer-joke removed (hidden human-exceptionalism, Seam #4).
- [ ] Title reflects the reversed thesis (forces isolate; destiny does NOT dominate).
- [ ] Every number traceable to a Phase-1..3 artifact in `runpod/2026-06-XX/`.
- [ ] Each claim re-checked against the harness output one final time before upload (§9 mine: no fabricated citation/number).

If any box can't be checked, the claim is held, not published. No partial credit (pre-reg rule).
