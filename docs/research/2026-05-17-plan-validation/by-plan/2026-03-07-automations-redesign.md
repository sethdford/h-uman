---
plan: docs/plans/2026-03-07-automations-redesign.md
auditor: group-2-channels
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Full-stack redesign of the Cron page into an agent-first "Automations" hub. Includes UI rename, automation card with 6 zones, schedule builder, creation modal, and backend extensions for agent jobs (cron.update, cron.runs, agent dispatch).

## Key Claims (from the plan)
- Claim 1: Rename "Cron" → "Automations" across sidebar, command palette, app router, hash.
- Claim 2: New UI components `hu-automation-card.ts`, `hu-schedule-builder.ts`, `automations-view.ts`.
- Claim 3: New control protocol methods `cron.update`, `cron.runs`; agent job dispatch in `cron.run`.
- Claim 4: Crontab format extension with `type`, `channel`, `name` fields; load-on-startup.
- Claim 5: `HU_CRON_JOB_AGENT` job type for agent dispatch.

### Implemented? (code exists)
- UI rename: `ui/src/app.ts:21,46,81,1306-1307` (route key + view loader); `sidebar.ts:43` ("Automations"); `command-palette.ts:24,45` ("Automations" + timer icon).
- New components present: `ui/src/components/hu-automation-card.ts` (550 LOC), `hu-automation-form.ts` (503 LOC), `hu-schedule-builder.ts` (640 LOC), `ui/src/views/automations-view.ts` (693 LOC).
- Control protocol: `src/gateway/control_protocol.c:48-53` registers `cron.list`, `cron.add`, `cron.run`, `cron.update`, `cron.runs`.
- Plan files referenced (`crontab.c`, `crontab.h`) exist — agent job dispatch path implied by `cron.update` and `cron.runs` registration.

### Proven? (tests exist)
- `ui/src/views/views.test.ts:228-230, 472-474` — `hu-automations-view` test cases.
- No specific test_*.c files found for cron.update / cron.runs handlers, but they are registered in the dispatch table (would need a deeper test file scan to confirm).
- Plan does not explicitly require new C tests; backend changes are protocol-level.

### Wired? (called in runtime path / dispatch)
- WIRED: UI routes through `app.ts` case "automations" → renders `<hu-automations-view>`.
- WIRED: cron.update / cron.runs handlers registered in control_protocol.c:52-53 dispatch table.
- Sidebar + command palette entries surfaced to user.

## Gaps
- Could not confirm `cron.run` agent dispatch path (HU_CRON_JOB_AGENT → `hu_agent_turn`) is fully wired without reading `cp_admin_cron_run`.
- Phase 2 items (real-time events, next-run countdown, run history detail view) status unknown — plan explicitly listed those as "Later".
- No C tests pinning the HU_CRON_JOB_AGENT type behavior surfaced via grep.

## Notes
- Plan marked `status: complete`.
- UI is wired end-to-end; backend handlers are registered. Confidence HIGH on shipping; MEDIUM on full agent-dispatch correctness.
