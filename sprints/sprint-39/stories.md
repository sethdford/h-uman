# Sprint 39 — Telemetry wire-up, quality gate 5×, per-channel G5, G7 lookahead

**Branch:** `sprint-39-telemetry-quality-gate`

## Goals

1. **Daemon periodic log** — every 100 turns, log `hu_guard_reject_stats_snapshot`.
2. **Quality gate 5×** — `hu_conversation_evaluate_quality` sets `needs_revision`
   when response/avg ratio > 5× (was 10×), aligning with post-mortem + G5.
3. **Per-channel G5** — imessage/cli/sms use 6× multiplier; others 8×.
4. **G7 lookahead 30→60** — widen parenthetical slack between name and verb.
5. **CI weekly audit** — macOS scheduled workflow for `scripts/audit-imessage-leaks.sh`.

## Definition of Done

- All stories shipped, 10339+N tests pass, tagged `v-sprint-39-close`.
