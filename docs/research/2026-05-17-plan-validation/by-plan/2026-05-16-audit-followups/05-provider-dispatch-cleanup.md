---
plan: docs/plans/2026-05-16-audit-followups/05-provider-dispatch-cleanup.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: NONE
proven: PARTIAL
wired: NONE
verdict: NOT_STARTED
confidence: HIGH
---

## Plan Summary
Eliminate `strcmp` provider routing in `onboard.c`, `voice.c`, `config_merge.c` by
extending `hu_provider_vtable_t` with `onboard_hints()`, `default_model()`, and
`supports_voice()` methods. AC-4: adding a provider should be a single-file change.

## Key Claims (from the plan)
- Claim 1: Vtable extended with three optional methods
- Claim 2: `src/onboard.c:275-371` strcmp loop replaced with vtable iteration
- Claim 3: `src/voice.c` Cartesia checks replaced with `vtable->supports_voice()`
- Claim 4: `src/config_merge.c` default-model strcmps replaced with `vtable->default_model()`
- Claim 5: Dummy no-op provider added in same PR as a demonstration of AC-4

## Evidence

### Implemented? (code exists)
- `grep -rn "onboard_hints\|supports_voice\|hu_provider_onboard_hints" src/ include/` → 0 hits
- Vtable extension not present
- `grep -c "strcmp(provider" src/onboard.c` → **8** (down from audit's 15 — partial trim,
  not the vtable-driven refactor the plan calls for)
- `src/voice.c` still has hardcoded `strcmp(..., "cartesia") == 0` at multiple sites
- `src/config_merge.c:137-139` still has explicit `strcmp(detected, "openai")`,
  `strcmp(detected, "anthropic")` chains

### Proven? (tests exist)
- The provider dispatcher safety contract IS proven via the M3 daemon-pattern test:
  `tests/test_provider_all.c:3071: test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`
  (cited in CLAUDE.md M3 row). This is a SEPARATE safety guarantee (adapter dispatch)
  from the strcmp cleanup that plan 05 promises.
- No test for AC-4 ("adding dummy provider only touches factory + new file")

### Wired? (called in runtime path / dispatch)
- N/A — vtable extension not implemented, so no wiring to verify

## Gaps
- Vtable still does not expose `onboard_hints`, `default_model`, `supports_voice`
- All 3 dispatch sites still use strcmp routing
- AC-1, AC-2, AC-3, AC-4 unmet
- The M5 "HuLa as Platform" thesis still pays the "add provider = edit three files" tax

## Notes
The CLAUDE.md M3 entry cites commit 028f4544 wiring the daemon-pattern fallthrough test
for the provider dispatcher's safety contract. That is a *different* issue from plan 05's
strcmp cleanup. Plan 05 is not started.
