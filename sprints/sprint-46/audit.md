---
title: "Sprint 46 — Sprint Auditor Report"
sprint: 46
branch: sprint-46-r5-finish
auditor: sprint-auditor (adversarial)
date: 2026-05-19
result: PASS_WITH_NOTES
---

# Sprint 46 — Adversarial Audit (consolidated)

## Audit history

| Pass | Result | Reason |
|------|--------|--------|
| 1 | FAIL | R5.3 missing 3 spec tests; R5.1 spec name renamed silently |
| 2 (post-fix `04df986d`) | **PASS_WITH_NOTES** | All 4 gaps closed; host-fixture note non-blocking |

## Story verdicts (final)

| Story | Verdict | Evidence |
|---|---|---|
| R5.1 — reply-latency ingest | PASS (post-fix) | `inbound_after_outbound_records_latency` (spec name alias) added in `tests/test_agent.c:140`. Original `dpo_record_inbound_arrival_computes_latency` in `tests/test_dpo.c` retained as the implementation-named variant. Both PASS. |
| R5.2 — alternatives column | PASS | All 6 ACs met. 2 spec'd tests PASS. 7 callers updated (build -Werror clean). |
| R5.3 — real p_seth_at_send | PASS (post-fix) | All 8 ACs met. The 3 spec'd integration tests added in `tests/test_agent.c` via helper-refactor of `hu_agent_internal_load_persona_eval`. Each test exercises the real production code path, not a mock. |

## Tests verified PASS individually by spec name

```
agent_init_with_persona_eval_model_present_loads_it    PASS
agent_init_with_missing_model_proceeds_without_failure PASS
record_outbound_with_p_seth_persists_column            PASS
inbound_after_outbound_records_latency                 PASS
```

## Quality checks (adversarial)

- ✅ No mocked-out-the-thing-under-test pattern (real loader, real sqlite, real wrapper)
- ✅ No tautological assertions (each test would fail under the broken implementation)
- ✅ Helper refactor is legitimate extraction, not test/source divergence (same helper used in production and test)
- ✅ Scope creep on `04df986d`: 5 files, all in scope of the audit fix
- ✅ Build -Werror clean

## Notes (non-blocking)

- **Host-fixture skip pattern**: AC-5 and AC-7 use `HU_SKIP_IF(!model_file_exists)` to gracefully skip on machines that don't have `/tmp/seth_speaker_id.json`. Correct pattern. The dev machine has the file so the tests actually executed and passed; CI machines without the fixture will skip rather than fail. If the model artifact ever becomes a release dep, a fixture stand-in should be checked into the repo.
- **Spec name alias for R5.1**: The original implementation-named test `dpo_record_inbound_arrival_computes_latency` is retained alongside the new spec-named `inbound_after_outbound_records_latency`. The alias resolves the audit's grep-find concern; the duplicate run cost is negligible. Either name resolves to the same behavior contract.

## Final verdict

**RESULT_sprint-auditor=PASS_WITH_NOTES**

Sprint 46 closes. Notes carry to retro.md for future-sprint discipline.
