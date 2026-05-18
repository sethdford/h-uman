---
plan: docs/plans/adr/2026-05-11-persona-eval-judge.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: MEDIUM
---

## Plan Summary
ADR accepting Option C: hybrid judge architecture. Frontier judge (Gemini 3.1 Pro via
Vertex AI ADC) for anchor / public benchmark / release candidates; local judge
(Gemma 3 27B via mlx-server) for nightly / PR-time / A/B; three-judge agreement
(local + frontier + deterministic perplexity) required for auto-promotion.

## Key Claims (from the plan)
- Claim 1: `eval_suites/persona/judge-prompts/` directory with versioned filenames
- Claim 2: Frontier judge calls via `src/providers/gemini.c` with ADC
- Claim 3: Three-judge promotion gate
- Claim 4: κ ≥ 0.6 inter-rater agreement requirement at lock-in
- Claim 5: `eval_suites/persona/README.md` documentation
- Claim 6: `docs/standards/ai/evaluation.md` updated with rollback policy section

## Evidence

### Implemented? (code exists)
- `ls eval_suites/persona/` → directory does NOT exist
- `eval_suites/` contains adversarial.json, fidelity.json, human_likeness.json,
  humor_engine.json, etc. — flat structure, no `persona/` subdirectory or
  `judge-prompts/` versioning
- `scripts/eval_humanness.py`, `scripts/eval_blinded_ab.py`, `scripts/eval_multiturn.py`
  exist — eval scripts ARE present but their judge-config wiring not audited here
- `src/providers/gemini.c` exists and uses Vertex AI ADC (confirmed by other plans)

### Proven? (tests exist)
- `tests/test_persona_fidelity_judge.c` exists (covers judge-related logic) ✓
- Not verified whether κ ≥ 0.6 gate is enforced

### Wired? (called in runtime path / dispatch)
- Eval scripts exist and presumably call providers; concrete frontier-vs-local routing
  per the ADR's three-judge contract not verified end-to-end

## Gaps
- `eval_suites/persona/` directory and judge-prompt versioning structure not in place
- `eval_suites/persona/README.md` doesn't exist
- κ ≥ 0.6 inter-rater agreement test/gate not verified

## Notes
The persona-eval infrastructure clearly EXISTS (scripts + fidelity tests + flat
eval_suites JSON), but the ADR's specific directory layout and versioning scheme is
not in place. The decision is broadly honored (Gemini via Vertex with ADC for
frontier; local for inner-loop) but the organizational structure deviates.
