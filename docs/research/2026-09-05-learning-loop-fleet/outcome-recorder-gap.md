---
title: production_outcomes recorder gap, 2026-08-04 → 09-01 — the daemon made no turns
date: 2026-09-05
status: root cause established (read-only investigation; no prod action taken)
---

`production_outcomes` (created `src/ml/dpo.c` `hu_dpo_init_tables`, written by
`hu_dpo_record_outbound` from `src/daemon.c` after every reactive reply) is the corpus
`scripts/m3_outcome_driver.py --export-only` turns into `~/.human/training-data/m3-outcomes.jsonl`
for the nightly LoRA retrain. It received **no row from 2026-08-04 01:54 UTC to 2026-09-01 23:31 UTC**.
Doctor, the nightly, and (once it existed) `check-learning-loops.sh` all reported health.

## TL;DR

**The recorder did not fail. Nothing called it.** The daemon completed zero reactive turns
in that window: it hung after the 04:35 nightly-fidelity crash on 08-04, was restarted
at 06:27 into a freshly quarantined (empty) memory store, never reached its first 60 s
maintenance tick, and from 08-08 08:05 the laptop was asleep or off until the 09-01 19:29
boot. The `messages` table, written by the same turn that calls the recorder, is equally
empty for the window; on every day with traffic the two tables agree to the row
(11/3/21/10/7/6/2 rows per day for 07-28 → 08-04 in both).

No C change is warranted: `hu_dpo_record_outbound` has no gate that changed, is not
behind `HU_ENABLE_RL_FULL`, logs its error at the call site (`daemon.c` "production_outcomes
record_outbound failed"), and no deploy or config edit happened at either gap edge.

## Timeline (all times EDT unless marked; evidence in brackets)

| When | What | Evidence |
|---|---|---|
| 08-02 15:50 | last daemon restart before the gap (plist backup `pre-grounding-20260802-155009`); binary built from main of 08-02 | `~/Library/LaunchAgents/*.bak-pre-grounding-*`, `config.json.bak.pre-grounding-*` |
| 08-03 21:54 | last `production_outcomes` row before the gap (id 430) | memory.db |
| 08-04 04:05–04:34 | nightly eval multi-turn step FAIL | `eval-nightly.log:122982-122985` |
| 08-04 04:34–04:35 | fidelity step loads a **second GLM-4.5-Air in-process beside :8741**, then `exit=134` (SIGABRT) after the first prompt — the identical mechanism documented on 09-03 (Metal OOM, `:8741` dies as a `?E` zombie, daemon frozen on a half-open socket) | `eval-nightly.log:122987-122998`; memory note `nightly_fidelity_loader_killed_prod` |
| 08-04 04:35–05:19 | blind-ab through the gateway: `HTTP 502 Bad Gateway` ×4 then `timed out` ×5, "server unhealthy", HARNESS FAIL 18/50 | `eval-nightly.log:123000-123208` |
| 08-04 **05:00:19** | daemon's **last 60 s maintenance tick** (prompt_budget snapshot) | doctor 08-04 07:43: "snapshot is 9825 seconds old"; cadence at `src/daemon/daemon_maintenance.c:93-96` is unconditional |
| 08-04 **06:27:47** | daemon (re)starts: open-sentinel present (previous instance died uncleanly) → `PRAGMA quick_check` non-ok → **memory.db quarantined, fresh empty store created** | `~/.human/memory.db.corrupt-1785839267` (ts 1785839267 = 06:27:47 EDT); `src/memory/engines/sqlite.c` quarantine path |
| 08-04 06:27 → 08-08 | **zero maintenance ticks, zero rows of any kind in the fresh store**; doctor reports the snapshot at exactly +86400 s per day (96221, 182621, 269025, 355422) from the same 05:00:19 origin | `doctor-nightly.log` 08-05..08-08; `memory.db.pre-merge-1788312235` (the fresh store, copied at merge time) has its first row at 09-01 23:31 UTC |
| 08-08 08:05 | last artifact from any launchd job (daily-feeds, daily-scrape; arena ran 4-hourly until 06:23; eval-nightly 04:05; doctor 07:43) | file mtimes under `~/.human/logs/`, `logs/arena/run-*.jsonl` |
| 08-09 → 08-31 | **nothing** under `~/.human` written; no doctor, arena, feeds, eval lines; no reboot record | `find ~/.human -newermt 08-09 ! -newermt 08-31` = 0 files; `last reboot` |
| 09-01 19:29 | boot (no preceding shutdown record); `.last_update_check` 19:30:10; daemon pid 997 up 19:31:12 | `last reboot`, `pmset -g log`, unified log |
| 09-01 19:31–19:56 | first turns in four weeks — the stale-cursor replay incident (fixed `75bbbfdcf`) — recorded into the fresh store: 12 outcomes, 12 assistant rows | `memory.db.pre-merge-*`; `docs/research/2026-09-01-sota-reassessment.md` addendum |
| 09-01 21:38 | fresh store merged back into the quarantined one (`ce5d5e400`) → the live DB has rows before and after the gap and none between | `memory.db.pre-merge-1788312235` mtime |

## Why the candidates the task listed are ruled out

- **Deploy that compiled the recorder out / gated it.** The reactive call site (`daemon.c`,
  "AGI-C1b — record outbound") sits under no `#ifdef`; the only gate is
  `agent->sota.sota_initialized`, set unconditionally at the end of SOTA init
  (`src/agent/agent.c`). No deploy happened between 08-02 and 09-01 (plist/config backups,
  git log). The `HU_ENABLE_RL_FULL` doctor error of 07-18 concerns the *tapback* collector and
  was cleared by 07-20.
- **`HU_IS_TEST`/config gate flipped.** Config diffs 07-29 → 08-02 → 09-02 touch only the
  reflection block, the adapter path, `nightly_lora_enabled`, and `auto_update`. None reach
  the recorder.
- **Schema migration failure.** `CREATE TABLE IF NOT EXISTS` ran fine on the fresh store —
  it holds `production_outcomes` with the 09-01 rows.
- **Channel that was quiet.** Only iMessage ever records (458/458 rows). chat.db shows
  30–100 inbound/day all August, but `message.date` is send time, not receipt time; iCloud
  back-filled the sleeping Mac on 09-01. The premise "memory.db `messages` grew in August" is
  false: 0 rows of either role 08-05 → 08-31.
- **Silent error return at the call site.** The call site logs `hu_log_warn` on any non-OK.
  The one silent path, `collector->db == NULL → HU_OK`, would also have left `messages`
  populated (different writer). It was not. (It is still a `reports-success-does-nothing`
  shape worth a one-shot init log; see follow-ups.)
- **Auto-updater.** `auto_update: "apply"` has been set since at least 05-29 and the latest
  GitHub release is `v2026.3.3` (March), so `hu_version_compare("0.5.0","2026.3.3")` reports
  an update on every daemon start past the 24 h marker (`service-loop.log` at the 09-01 boot:
  "Update available: 0.5.0 -> v2026.3.3. Downloading..."). The apply **failed** every observed
  time ("Auto-update failed" ×3 in the 09-02/03 log; the release has no `sha256sums.txt`, so
  `verify_sha256` aborts) and it renames atomically, so it did not overwrite or hang the
  daemon. Turned off in config on 09-02 02:07. Not the cause; a live landmine.

## What is not established

- What the 06:27:47 instance did after the quarantine. It logged to
  `service-loop-error.log`, which was truncated before the 09-03 rotation (the rotated copy
  starts 09-02), and the unified log does not reach back to 08-04. It never produced a
  maintenance tick, so it either hung during startup (the `:8741` listener held by a zombie
  accepts connects and never answers; `CURLOPT_CONNECTTIMEOUT` has since been added in
  `src/core/http.c:214`) or exited within 60 s in a way `KeepAlive{SuccessfulExit:false}`
  did not restart (an exit 0 is never restarted). The 87-minute gap 05:00 → 06:27 fits the
  first instance blocking on 600 s HTTP timeouts before dying.
- Who restarted it at 06:27 (launchd KeepAlive after a crash, or a script).

## Why nothing surfaced it

- Doctor **did** say it: `prompt_budget ... daemon may not be flushing` was the one error
  on 08-04..08-08. It was a `fails=1` line in a log nobody reads when the laptop is closed.
- `check-learning-loops.sh` did not exist until 09-01/02, and its `adapters` line measured
  retrain output, not the corpus feeding it.
- The nightly retrain's export would have written 0 new pairs and exited 0.

## What changed (this commit)

`scripts/check-learning-loops.sh` gains an `outcomes` line that reads `~/.human/memory.db`
read-only (`file:...?mode=ro`) and compares the two artifacts of the same turn over the
last `HU_LOOP_OUTCOMES_MAX_DAYS` (default 3) days:

- assistant turns in `messages` > 0 and `production_outcomes` rows = 0 → **DEAD** (the
  recorder is not firing; a fresh store with no `production_outcomes` table counts as 0)
- both 0 → **NOTE** naming the daemon (this incident's shape; doctor's prompt_budget check
  owns daemon liveness)
- otherwise OK with both counts

Hermetic tests O1–O7 in `scripts/test_check_learning_loops.sh` (throwaway sqlite file, never
the real store; verified to FAIL against the pre-change checker). Against the live store
today: `OK outcomes: 5 row(s) from 5 assistant turn(s) in the last 3d`.

Honest limit: had this line existed in August it would have printed **NOTE**, not DEAD,
because the daemon made no turns. The DEAD half catches the recorder-only failure the task
suspected; the NOTE half exists so the "nothing to record" case is a visible fact rather
than silence.

## Follow-ups (not done here; one concern per change)

1. `hu_dpo_record_outbound` returns `HU_OK` with a NULL db and the daemon never says so —
   add a one-shot init log per `.claude/rules/silent-config-gated-subsystems.md`.
2. `hu_update_maybe_check` runs inside the long-lived daemon and will downgrade to a March
   release the day someone uploads `sha256sums.txt`; refuse to apply when the running binary
   carries `HU_BUILD_SHA`, or drop "apply" from the service path.
3. A daemon that never reaches its first maintenance tick after start should be a doctor
   error in its own right (startup deadline), independent of the snapshot age.
4. Doctor findings on a sleeping laptop need a delivery path that survives the lid closing
   (the 6-hour artifact-keyed watchdog from the 09-01 reassessment is the start).
