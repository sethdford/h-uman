# Sprint 12 — Decision Log

Stakeholder: Seth Ford
Date: 2026-05-17

## D1 — OQ-12.1: Holdout size + Gemma variant for US-12.4

**Decision:** 30 rows / Gemma-4-E2B (PO recommendation, option A).

- Holdout: keep `~/.human/private/yntp_holdout_30.jsonl` at 30 rows for Sprint 12. Sprint 13 grows it if 95% CI is unacceptably wide.
- Model: Gemma-4-E2B (preserves Sprint 11 design continuity; fast inference for iteration).
- E4B exploration deferred to Sprint 13.

**US-12.4 implementer must:**
- Report point estimate AND 95% CI on `delta_ll`
- Note the 30-row sample size as a limitation in the published interpretation
- Not silently grow the holdout; if a row is malformed, surface the schema-validation failure rather than imputing

## D2 — OQ-12.2: KL drift probe set source (defers to PO recommendation)

**Decision:** Synthetic 200-prompt probe set, committed to repo at `tests/fixtures/kl_probe_200_synthetic.jsonl`.

- PII-free, deterministic, runnable in CI.
- US-12.3 implementer generates the synthetic set as part of the story (style-diverse, length-diverse, ~200 distinct prompts).
- A real PII-redacted probe set is deferred to Sprint 13 (would need Seth's curation time, blocks no Sprint 12 work).

## D3 — OQ-12.3: Twin-2K-500 real-labeling deferral (extend D2 from Sprint 11)

**Decision:** Extend Sprint 11's D2 — Twin-2K-500 real 50q labeling still deferred. No Twin-2K-500 story in Sprint 12.

- The synthetic-10 fixture from Sprint 11 US-11.10 remains the test seam.
- Real labeling lands in Sprint 13 when Seth has ~2-3h of manual time, OR when an external annotator is contracted.

## D4 — OQ-12.4: Negative-result protocol for US-12.4

**Decision:** Ship the negative result + run ORPO comparison (option B from PO).

- If `delta_ll(DPOP+DoRA, base+system-prompt) < 0` on the real YNTP run, the sprint does NOT pause for re-design.
- Sprint 12 becomes an A/B experiment: produce ≥3 data points (base, DPOP+DoRA, ORPO) against the same 30-row holdout in the same sprint.
- US-12.4 evidence must include all 3 numbers, the gate verdict for each, and an honest interpretation that does not bias toward positive findings.
- US-12.5 (production ORPO `train_step`) is therefore Wave 1 alongside US-12.4 (not Wave 2 as PO drafted) because it's now load-bearing for US-12.4's A/B protocol.

**Implication for wave plan:** scrum-master should promote US-12.5 from P1/Wave 2 to P0/Wave 1 alongside US-12.4. The dependency chain becomes: Wave 0 (US-12.1, US-12.2, US-12.3) → Wave 1 (US-12.4 + US-12.5 parallel).

## Cross-cutting note for Sprint 12 execution

Sprint 11 retro flagged 3 hookify candidates that affect Sprint 12 implementer behavior. None are blocking, but the lead should watch for the same patterns:

1. **Critic truncation** — if any Phase 2 critic returns without a `RESULT_critic=...` line in the last 200 chars, resume via SendMessage with focused verdict prompt.
2. **Worktree base-ref drift** — implementer prompts must explicitly say "fetch + reset to `origin/sprint-12-prove-the-metric`"; Sprint 11 US-11.5 branched from `main` and produced ~1,875 LOC of inherit-state noise.
3. **"Deferred per design §X" framing** — if a Sprint 12 commit or followup claims deferral, the lead must verify the cited section actually defers. Sprint 11 US-11.4's "deferred per §1.4" was wrong; auditor caught it.
