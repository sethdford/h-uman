# Sprint 4 Designs — Summary

Scope: US-4, US-5, US-6, US-9, US-10 (PO option b). US-7, US-8 deferred to Sprint 5.

## Risk tier
| Story | Tier | Why |
|---|---|---|
| US-4 (chain caching) | **High** | 7+ call-site migration in hot daemon/agent paths; lifetime correctness on persona owns chain. Largest blast radius. |
| US-5 (telemetry) | **Medium** | New observer enum value risks `-Werror=switch` breaks across existing observer implementations; string lifetime in event payload. |
| US-6 (E2E test) | **Medium** | `hu_daemon_stream_ctx_t` construction under test is unknown; AC-6.3 deletion-experiment is manual. |
| US-9 (DoD annotation) | **Low** | Comment-only source change + markdown edits. No behavior. |
| US-10 (hookify rule) | **Low** | Tooling-only; hook bypassable; no runtime impact. |

## Wave / sequencing (matches PO guidance)
- **Wave 1 (parallel)**: US-9 + US-10. Both XS, disjoint files (`daemon.c` comments + sprint docs vs `.claude/rules/` + `scripts/` + `.githooks/`). Land first to clear audit + install the test-reference guardrail before implementer waves begin.
- **Wave 2**: US-5. S-sized, no dependency on US-4 or US-6. Touches `observer.h`, `daemon.c`, `agent_{turn,stream}.c` — minor line overlap with US-4 in wave 3, but additive only.
- **Wave 3 (sequential)**: US-4 → US-6. US-6's test exercises the cached chain; if dispatched in parallel, US-6 must rebase post-US-4 merge. Flag if US-4 runs long.

## Total estimated effort
**XS + XS + S + M + M ≈ 2.5–3.5 engineer-days** including tests, ASan validation, and PR review.

## Open items
- US-4 AC-4.3 scope: confirm migration of all 6 `daemon.c` sites (audit says 5). Default = all 6.
- US-6 mock-provider literalism: option-2 design omits mock provider; PR must document deviation.
- US-10 hook scope: enforce on `--diff-filter=A` (new only) vs `AM` (any modified). Default = A.

RESULT_tech-lead=DESIGNED
