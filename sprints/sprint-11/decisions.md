# Sprint 11 — Decision Log

Stakeholder: Seth Ford
Date: 2026-05-16

## D1 — OQ-11.6.1: YNTP holdout fixture policy

**Decision:** Hybrid (option C from the plan).

- `tests/fixtures/yntp_synthetic_5.jsonl` — fully synthetic, committed to repo, runs in CI; no PII
- `sprints/sprint-11/evidence/yntp_sample_10.jsonl` — 10-row redacted sample from real chat.db, committed to repo for human eyeball + Seth's review
- `~/.human/private/yntp_holdout_30.jsonl` — git-ignored, lives only on Seth's machine; 30-row PII-redacted real fixture for the production metric

**Evaluator behavior:** picks the private fixture if `HU_YNTP_HOLDOUT` env var is set; falls back to synthetic-5 in CI and clean-checkout. AC-11.6.3 (Sprint 8 broken adapter must FAIL the gate) must hold against BOTH fixtures.

**US-11.6 implementer:** must add `.gitignore` entry for `~/.human/private/` AND a pre-commit hook check that fails if `yntp_holdout_30.jsonl` ever lands in the repo.

## D2 — OQ-11.10.1+2: Twin-2K-500 scope

**Decision:** Ship code + 10-question synthetic demo this sprint; defer real 50-question labeling to Sprint 12.

- US-11.10 ships: `human ml twin-eval --protocol forced-choice` CLI + `tests/fixtures/twin2k_synthetic_10q.jsonl` + all AC/tests pass against synthetic
- Deferred: `~/.human/private/twin2k_seth_50q.jsonl` (~2-3h of Seth's manual labeling time) and the real behavioral consistency measurement

**US-11.10 implementer:** must NOT attempt to label real data on Seth's behalf. The code path must work end-to-end on synthetic; the real fixture is a future Seth-action.

## D3 — Sprint 11 execution scope

**Decision:** Run full 4 waves with all quality gates (verifier + critic + aspect-panel-for-MEDIUM+).

- Wave 0 → Wave 1 → Wave 2 → Wave 3 → Phase 3 review → Phase 4 audit → Phase 5 retro → Phase 6 tag `v-sprint-11-close`
- Estimated budget: $90-110, ~4-7h wall-clock
- Seth can interrupt at any wave boundary

## Cross-cutting note

The agent-tuner patches from Sprint 7 retro CHANGE-2 were effective in this sprint:
- 9 of 10 tech-leads returned RESULT in the same response as artifact write (no nudges needed)
- 1 (US-11.4 DPOP) paused mid-task and needed a SendMessage nudge

The 9/10 success rate is a massive improvement over Sprint 7's ~30% rate. Sprint 11 retro should note this and recommend any further patches if the 1 remaining pause-pattern recurs.
