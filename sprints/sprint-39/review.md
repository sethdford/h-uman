# Sprint 39 Review

**Tag:** `v-sprint-39-close`  
**Tests:** 10343/10343 pass, 0 ASan errors (+4 tests)

## Shipped

1. **Daemon telemetry** — every 100 turns, `hu_log_info` with `hu_guard_reject_stats_snapshot` counts.
2. **Quality gate 5×** — `hu_conversation_evaluate_quality` sets `needs_revision` when response/avg ratio > 5 (was 10).
3. **Per-channel G5** — `hu_guard_length_anomaly_mult_for_channel`: imessage/cli/sms 6×, others 8×; wired in `agent_stream.c` (×2) and `agent_turn.c`.
4. **G7 lookahead 60** — `HU_GUARD_PERSONA_LOOKAHEAD` 30→60; regression for long parenthetical before ` is a `.
5. **CI audit** — `scripts/audit-imessage-leaks.sh --self-test`; `.github/workflows/imessage-leak-audit.yml` (weekly macOS + self-test).

## Post-mortem

Action items #13, #17, #18 marked done; fix matrix updated for G5/G7.
