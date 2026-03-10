# Automations Page Redesign

**Date:** 2026-03-07
**Status:** Approved
**Scope:** Full-stack redesign of the Cron page into an agent-first "Automations" hub

## Problem

The current Cron page is a minimal cron-expression-and-shell-command list. It exposes none of the
backend's agent scheduling capabilities, has no edit flow, no pause/resume, no run history, no
schedule builder, no delete confirmation, and uses raw cron syntax that most users can't write.
The backend has 19 identified issues including agent jobs executing as shell commands and jobs not
surviving gateway restarts.

## Vision

Rebrand "Cron" to **"Automations"** — a page that makes agent scheduling feel like a first-class
product feature. The AI agent proactively running tasks on a schedule is the killer feature that
Apple/Google Shortcuts and traditional cron tools don't offer.

## Design Decisions

### Page Architecture

- Rename "Cron" to "Automations" across sidebar, command palette, URL hash, and app router.
- Two-tab layout: **Agent Tasks** (default) and **Shell Jobs** (power users).
- Stats bar at top: active count, paused count, runs today, failure count.
- "New Automation" button adapts to active tab.
- Icon: `icons.timer` everywhere (fixing current inconsistency with `icons.clock`).

### Automation Card (Dashboard Card)

Each automation is a rich monitoring card with six zones:

1. **Header**: Pause/resume toggle (left), name (bold), overflow menu [Edit | Delete] (right).
2. **Schedule**: Human-readable cron expression ("Every day at 8:00 AM").
3. **Prompt/Command**: Agent prompt (muted block, 2 lines, expandable) or shell command (mono).
4. **Status Trio**: Three compact badges:
   - Channel badge with icon (agent jobs only)
   - Next Run countdown ("in 2h 14m")
   - Last Status (checkmark/x + time)
5. **Run History Strip**: Last 7 runs as colored dots (green/red/gray). Hover for details.
6. **Footer**: "Run Now" button (left), creation date (right).

Card states: Active (green), Paused (dimmed + orange), Error (red accent), Running (pulse).

Delete requires confirmation modal with run count context.

### Creation Flow

Modal form (using `hu-modal` for focus trap, Escape, aria):

- **Prompt** (agent): "What should the agent do?" — textarea
- **Channel** (agent): "Where should it respond?" — dropdown from `channels.status`
- **Schedule**: Visual builder with presets (Every day, Every hour, etc.) + time picker.
  Shows raw expression with "edit" link for advanced users.
- **Run mode**: Radio — "Run once" / "Recurring"
- **Name**: Optional, auto-suggested from prompt if empty.

Shell variant: command input + schedule builder + name.

Edit uses the same modal pre-populated.

#### Schedule Presets

| Preset                  | Expression           |
| ----------------------- | -------------------- |
| Every minute            | `* * * * *`          |
| Every 5 minutes         | `*/5 * * * *`        |
| Every hour              | `0 * * * *`          |
| Every day at {time}     | `0 {hour} * * *`     |
| Every weekday at {time} | `0 {hour} * * 1-5`   |
| Every Monday at {time}  | `0 {hour} * * 1`     |
| Every month on 1st      | `0 {hour} 1 * *`     |
| Custom                  | raw expression input |

### Backend Changes (Phase 1)

#### Control Protocol

| Method        | Change          | Fields                                                                                   |
| ------------- | --------------- | ---------------------------------------------------------------------------------------- |
| `cron.list`   | Expand response | Add `type`, `channel`, `last_status`, `paused`, `created_at`                             |
| `cron.add`    | Expand params   | Add `type` (shell/agent), `prompt`, `channel`                                            |
| `cron.update` | NEW             | `id` + optional `expression`, `command`/`prompt`, `enabled`, `paused`, `name`, `channel` |
| `cron.run`    | Fix dispatch    | Agent jobs trigger agent turn, not shell                                                 |
| `cron.runs`   | NEW             | Return last N runs for a job ID                                                          |

#### Persistence Fix

- On gateway startup, load crontab entries into the scheduler.
- `cron.remove` removes from both scheduler and crontab.
- Extend crontab format to support `type`, `channel`, `name` fields.

#### `cron.run` Agent Dispatch

When `job.type == HU_CRON_JOB_AGENT`:

- Call `hu_agent_turn(agent, job->command, ...)` (command holds the prompt)
- Publish response to the job's channel via bus
- Record run in history

### Phase 2 (Future)

- Compute `next_run_secs` properly in scheduler tick.
- Event bridge: broadcast `cron.job.started/completed/failed` for real-time UI updates.
- Auto-refresh in the UI via events.
- `cron.pause` / `cron.resume` convenience methods.
- Human-readable cron expression parser (in JS, for display).
- Channel-specific output formatting.

## Implementation Phases

### Phase 1 (This Session)

Backend:

1. Expand `cron.list` response with missing fields.
2. Add `cron.update` control protocol method.
3. Expand `cron.add` for agent job support.
4. Fix `cron.run` to dispatch agent jobs correctly.
5. Add `cron.runs` control protocol method.
6. Load crontab into scheduler on gateway startup.
7. Fix `cron.remove` to also remove from crontab.

UI:

1. Rename Cron to Automations (sidebar, command palette, app router, hash).
2. Implement tab bar (Agent Tasks / Shell Jobs).
3. Stats bar component.
4. New automation card with all six zones.
5. Creation modal with prompt, channel picker, schedule builder, name.
6. Edit modal (reuses creation modal).
7. Pause/resume toggle on cards.
8. Delete confirmation modal.
9. Run history strip on cards.

### Phase 2 (Later)

- Real-time event updates
- Next-run countdown timer
- Run history detail view
- Schedule expression preview ("Next: Tomorrow at 8:00 AM, then...")

## Risk Assessment

- **Medium**: Crontab format extension — backward compatible if we add optional fields.
- **Medium**: Agent dispatch in `cron.run` — needs access to the agent instance from
  the control protocol handler (same pattern as `gw_agent_on_message`).
- **Low**: UI rename — search-and-replace across sidebar, palette, router.
- **Low**: Card design — all tokens and components exist.

## Files Affected

Backend:

- `src/gateway/control_protocol.c` — cron handlers
- `src/crontab.c` — format extension + load-on-startup
- `src/main.c` — wire crontab loading
- `include/human/crontab.h` — struct extension

UI:

- `ui/src/views/automations-view.ts` — replaced former `cron-view.ts` (now deleted)
- `ui/src/app.ts` — routing rename
- `ui/src/components/sidebar.ts` — label/icon
- `ui/src/components/command-palette.ts` — label/icon
- New: `ui/src/components/hu-schedule-builder.ts`
- New: `ui/src/components/hu-automation-card.ts`
