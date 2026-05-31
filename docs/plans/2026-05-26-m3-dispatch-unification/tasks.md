# M3 Dispatch Unification — Tasks

Tasks are ordered such that each is independently shippable and tested.
A task is "done" when its acceptance criterion in
[requirements.md](requirements.md) is verified by the test pinned in
[design.md](design.md) AND the full suite passes.

## T1 — Add `hu_proactive_compose_inputs_t` + the `_ex` extension function

**Files**
- `include/human/agent/init_proposer.h` — declare the struct + new function.
- `src/agent/init_proposer.c` — implement `_ex` as a thin wrapper that
  copies relevant inputs into the bundle, then delegates to the
  existing `hu_init_proposer_tick_with_provider`.

**Test**
- `tests/test_init_proposer_compose.c` — 6 tests: each context source
  populated → present in rendered prompt; each empty → cleanly absent.

**Estimate** ~150 LOC + 6 tests. 0.5 day.

**AC referenced**: AC-2.

## T2 — Move validator chain into init_proposer's send wrap

**Files**
- `src/agent/init_proposer.c` — after `hu_init_proposer_evaluate_decision`
  returns FIRED, run `hu_response_guard_check_ex` on `decision.draft`,
  retry on REJECT, log DPO negative pair on rejection regardless of
  retry outcome.
- Existing helpers reused: `hu_response_guard_retry_slim_with_identity`,
  `hu_response_guard_log_dpo_negative`.

**Test**
- `tests/test_proactive_dispatch_validator_chain.c` — inject
  G9-tripping draft via mock provider; assert REJECT → retry → either
  succeed or downgrade to LLM_ERROR; assert DPO negative pair captured.

**Estimate** ~120 LOC + 4 tests. 0.5 day.

**AC referenced**: AC-3, AC-5 (DPO capture surface).

## T3 — Add `cfg->proactive.use_unified_dispatch` flag

**Files**
- `include/human/config.h` — add field.
- `src/config/config_merge.c` — default false.
- `src/config/config_parse.c` — parse from JSON.

**Test**
- `tests/test_config_extended.c` — pin field defaults to false; pin
  parse from `{"proactive": {"use_unified_dispatch": true}}`.

**Estimate** ~50 LOC + 2 tests. 0.5 day.

**AC referenced**: AC-6.

## T4 — Wire daemon_proactive scheduler loop to call the `_ex` function

**Files**
- `src/daemon.c` — in the proactive-send block (~line 2086+), branch
  on `cfg->proactive.use_unified_dispatch`. Legacy branch unchanged;
  new branch builds the compose-inputs struct, calls `_ex`, on FIRED
  sends via the channel vtable + records the throttle.

**Test**
- `tests/test_proactive_dispatch_unified.c` — walk 3-contact tick
  cycle with a mock provider returning FIRED/SKIP/GATED_BUDGET for
  each contact respectively; assert only the FIRED contact's channel
  vtable sees a send call.

**Estimate** ~200 LOC + 3 tests. 1 day.

**AC referenced**: AC-1, AC-4, AC-5.

## T5 — Move `hu_daemon_callback_content_is_safe` into the compose-inputs flow

**Files**
- `src/agent/init_proposer.c` — when `inputs.memory_context` is non-NULL
  and `inputs.content_is_safe` is non-NULL, run the predicate on each
  memory entry before inclusion in the prompt.
- `src/daemon.c` — pass `hu_daemon_callback_content_is_safe` in the
  inputs struct.

**Test**
- `tests/test_init_proposer_compose.c` — add 2 tests: unsafe memory
  entry (contains first-person pronoun) is filtered; safe entry passes
  through.

**Estimate** ~50 LOC + 2 tests. 0.5 day.

**AC referenced**: AC-2 (memory safety subclause), Risk 3 mitigation.

## T6 — A/B observation period (1 week)

**Files**: none.

**Activity**
- Deploy with `use_unified_dispatch = true` for a single contact
  (Seth's own self-chat).
- Watch `~/.human/logs/service-loop-error.log` for FIRED rate +
  retry-rescued vs retry-thrashed (Sprint 41 follow-up #3 telemetry).
- Compare against the legacy path's same-week non-skip rate.

**Pass criterion**
- FIRED rate ≥ 95% of legacy non-skip rate (within noise).
- retry-thrashed rate ≤ legacy `response_guard REJECT` rate.
- No new ERROR-level log lines from init_proposer's send wrap.

**Estimate** Wall-clock 7 days; engineering time ~1 hour to set up the
log-scrape script.

**AC referenced**: AC-7 (no regression).

## T7 — Flip default to true

**Files**
- `src/config/config_merge.c` — change default from false to true.

**Test**
- `tests/test_config_extended.c` — update default-value test.

**Estimate** Trivial; gated on T6 result. 0.25 day.

## T8 — Delete the legacy proactive composition path

**Files**
- `src/daemon.c` — remove the legacy branch of the
  `use_unified_dispatch` conditional. The flag is now dead; remove it
  too in a follow-up.
- `src/daemon/daemon_proactive.c` — keep the context-builder helpers (they
  feed compose-inputs); remove the old prompt-assembly entry point.
- `include/human/config.h` — remove `use_unified_dispatch` field.

**Test**
- All proactive-dispatch tests should still pass without modification
  (the unified path is now the only path).
- A new audit test: grep for `hu_agent_turn` in `src/daemon.c`
  proactive-send block and assert zero matches.

**Estimate** ~80 LOC removed + 1 audit test. 0.5 day.

**AC referenced**: AC-1 (final form), AC-7 (no regression).

## Totals

- Engineering: ~650 LOC added (~400 implementation, ~250 tests),
  ~80 LOC removed. ~3.25 engineering-days.
- Calendar: ~10 days (engineering + 1-week A/B).
- Test count delta: +17 tests across 3 new test files.

## Pre-conditions

- Sprint 41 fully landed (G9 detector + retry telemetry + DPO logger +
  arbiter unification + config kill switch + per-channel disable list).
- Sprint 59 Phase C feed scoping landed (so memory/feeds contexts are
  already per-contact-scoped — the compose-inputs struct doesn't need
  to re-scope).
- `hu_response_guard_record_g9_retry_outcome` telemetry shipping to
  production for ≥3 days so the T6 A/B has baseline comparison data.

## Post-conditions

- Every proactive outbound goes through init_proposer.
- DPO negative-pair capture covers 100% of proactive rejections.
- Confidence-threshold gating + validator-chain shaping uniform across
  reactive and proactive surfaces.
- `daemon_proactive` becomes a thin scheduler module (compose +
  context-builder helpers, no direct send path).

## Status

**T0 (this spec)** — done 2026-05-26 (you are reading it).

**T1–T8** — not started. Pick up in a fresh session with `/spec
docs/plans/2026-05-26-m3-dispatch-unification/`.
