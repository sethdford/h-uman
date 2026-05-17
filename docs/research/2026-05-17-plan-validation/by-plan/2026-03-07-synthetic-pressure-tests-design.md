---
plan: docs/plans/2026-03-07-synthetic-pressure-tests-design.md
auditor: group-2-channels
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: PARTIAL
verdict: SHIPPED_UNWIRED
confidence: HIGH
---

## Plan Summary
Design doc for `human_synthetic` — a separate executable that drives end-to-end CLI, gateway HTTP, WebSocket, agent, and pressure tests using Gemini for scenario generation. Forks workers for load testing.

## Key Claims (from the plan)
- Claim 1: `human_synthetic` separate executable, not in `human_tests` (avoids Gemini dependency in unit tests).
- Claim 2: Source layout in `tests/synthetic/` — `synthetic_main.c`, `synthetic_gemini.c`, `synthetic_cli.c`, `synthetic_gateway.c`, `synthetic_ws.c`, `synthetic_agent.c`, `synthetic_pressure.c`, `synthetic_regression.c`.
- Claim 3: Fork-based pressure with workers piping metrics to parent.
- Claim 4: CLI flags `--binary`, `--cli-only`, `--port`, `--count`, `--regression-dir`, `--replay`, `--pressure-only`, `--concurrency`, `--duration`.
- Claim 5: Gateway lifecycle managed via temp config in `/tmp/hu_synth_XXXXXX/.human/config.json`.

### Implemented? (code exists)
- `tests/synthetic/` directory present with: `synthetic_main.c`, `synthetic_gemini.c`, `synthetic_cli.c`, `synthetic_gateway.c`, `synthetic_ws.c`, `synthetic_agent.c`, `synthetic_pressure.c`, `synthetic_regression.c`, `synthetic_harness.h`, `synthetic_utils.c`.
- Files map 1:1 to the design doc's architecture diagram.

### Proven? (tests exist)
- The harness IS the test — it exercises real binary + gateway.
- No `tests/test_synthetic_*.c` files (the harness lives in `tests/synthetic/` as a separate executable, not in the standard test runner).
- No checked-in regression dumps or success-metric artifacts.

### Wired? (called in runtime path / dispatch)
- Built via `HU_ENABLE_SYNTHETIC=ON` CMake option (per plan section "Build").
- Not part of default `./build/human_tests` runner — runs against Gemini API.
- Not in CI standard matrix per project CLAUDE.md ci.yml description (no `human_synthetic` listed there).

## Gaps
- Cannot confirm fresh end-to-end runs without GEMINI_API_KEY-driven manual invocation.
- No artifacts (regression dir, latency baseline) checked into repo.

## Notes
- Plan marked `status: complete` and "Implemented".
- This is the predecessor harness for the channel-tests work — same fork-based pressure pattern.
