---
title: Prospective memory v2 — gates, backfill, harness and promotion
created: 2026-09-30
status: operator-facing
spec: docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md
---

# Prospective memory v2

How h-uman keeps promises and follows up, and how to move it from off to live.
Design: [spec](../superpowers/specs/2026-09-30-prospective-memory-v2-design.md).
Build: [plan](../superpowers/plans/2026-09-30-prospective-memory-v2.md).

Every intention lives in one typed store, `prospective_memories` (cue_kind
`keyword` or `time`, status `pending` / `surfaced` / `done` / `canceled` /
`expired`). A keyword intention is cued by matching text in an inbound
message; a time intention is cued by `due_at`. Before anything surfaces, code
filters it (`hu_prospective_filter`: pending only, a 3-day grace past
`due_at`, at most one time intention surfaced per contact per local day,
never in a group chat, never to the owner's own handle). An eligible
intention goes to a local-model fire-time check (`hu_prospective_decide`),
which returns `fire`, `already_resolved`, `cancel` or `not_now` — a model
error, an unparseable answer, `not_now` and `parse_fail` all keep the item
pending (fail toward silence). An intention that fires is `surfaced`; it
becomes `done` only after the delivered reply actually carries at least half
of its key content words (`hu_prospective_reply_uses_action`), otherwise it
gets one more attempt before `expired`.

## Gate 1: `HU_PROSPECTIVE` (reactive keyword path)

Read once per reactive turn by `hu_daemon_prospective_reactive`
(`src/daemon/daemon_prospective.c`), acting on the keyword cue. Parsed by the
shared `off|shadow|live` gate parser (`hu_gate_mode_from_env`, case
insensitive; `on`/`live`/`1` all mean LIVE; any unrecognized value fails
closed to OFF); default OFF. The first call per process logs one banner line.

| Value | Effect |
|---|---|
| `off` (default) | Today's directive, byte-identical: `[PROSPECTIVE MEMORY: Remember to: <action> (triggered by: <cue>) | …]`, marked `fired=1` at render. Banner: `prospective v2 disabled (HU_PROSPECTIVE unset or off); set HU_PROSPECTIVE=shadow|live to run the fire-time check`. |
| `shadow` | Today's directive unchanged. Filter + Decide also run read-only (`hu_prospective_v2_run(..., apply=false, ...)`), up to 3 judge calls on a turn whose text cued a keyword intention. Logs `prospective shadow: candidates=… fire=… resolved=… cancel=… not_now=… parse_fail=… judge_err=… expired=… capped=… write_err=…`, one `prospective shadow item: id=… verdict=…` line per judged intention, and — after the reply is delivered to the same contact — `prospective shadow uptake: would_fire=… used=…`. |
| `live` | The v2 pass replaces the directive: `[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: <action> | …]`. A fired intention becomes `surfaced`; on delivery it is `done` only if the reply carries its key terms, otherwise one more attempt before `expired`. A judge error or unparseable verdict shows nothing (returns `NULL`) rather than guessing. |

## Gate 2: `HU_PROSPECTIVE_TIME` (time cues on the proactive tick)

Read once per proactive tick by the two producers in
`src/daemon/daemon_prospective_time.c` —
`hu_daemon_prospective_commitment_ctx` (the commitment follow-up lines) and
`hu_daemon_prospective_due_followups` (the proposer's `due_followups`
section) — both invoked from `hu_service_run_proactive_checkins`. Same gate
parser, default OFF, one banner line per process.

| Value | Effect |
|---|---|
| `off` (default) | The legacy producers, unchanged. |
| `shadow` | Legacy output unchanged. One read-only v2 time pass per contact per local day (a per-contact hash slot dedupes repeated ticks on the same day), logged as `prospective time shadow: …` counts plus one `prospective time shadow item: id=… verdict=…` line per judged intention. |
| `live` | `commitment_ctx` returns nothing (commitments are mirrored into the typed store instead), and `due_followups` returns the per-contact due set as `- <action>\n` lines, at most one per contact per day. The surfaced item is settled against the proactive text actually sent (`hu_daemon_prospective_time_after_send`). |

`HU_PROSPECTIVE_TIME` stays in `shadow` until `HU_PROSPECTIVE` itself has
been promoted through the §3 bar (spec §4.4) — see the rollout below.

## Rollout (spec §6)

1. **Merge with both gates off. Deploy** with `scripts/install-human-daemon.sh`
   and verify with `scripts/verify-deploy.sh <commit>`. The daemon's first
   open of `memory.db` runs the v2 migration (new columns, `fired → status`
   backfill for existing rows).
2. **Backfill and harness.**
   - `python3 scripts/prospective_backfill.py` is a dry run against
     `~/.human/memory.db` by default: it reports exact counts on stdout and a
     JSON manifest under `~/.human/logs/prospective-backfill-<ts>.json`
     (0600), and leaves the database untouched (the C side rolls its
     transaction back).
   - `python3 scripts/prospective_backfill.py --write` first backs up
     `memory.db` (SQLite online backup API, 0600 file under
     `~/.human/backups/`, 0700 dir) and refuses (exit 2, database untouched)
     if the backup fails — then applies. A dated commitment or follow-up
     overdue by more than 14 days imports as `expired`; overdue by up to 14
     days imports `pending`, re-anchored to the backfill time. An item found
     expired also retires its own ledger row in the same transaction (a
     commitment to `expired`, a follow-up to `sent=1`), because
     `agent_turn.c` and `proactive.c` read the ledger whatever the gates
     say; `ledger_retired` counts them (the dry run reports the exact number
     a `--write` would retire). A re-run no longer sees those rows, so its
     `commitments_seen` / `followups_seen` / `skipped_existing` are lower by
     them. Exit 2 means
     refused (missing/unmigrated database, missing `human` binary, unusable
     manifest dir — checked before anything is written); exit 3 means the
     backfill itself ran (and, with `--write`, the database was written) but
     the manifest could not be written, with the counts printed to stdout as
     a fallback.
   - `python3 scripts/pm_bench_local.py` runs 56 scripted scenarios (clean
     positive, overloaded with 4 pending distractors, silent negative,
     cancellation, reschedule, cross-day) against the real probe
     (`human prospective init` / `probe --full`), judged by the local model
     by default. Verdict PASS (exit 0) needs Set-F1 ≥ 0.80, a silent-negative
     false-alarm rate ≤ 5% and a cross-day miss rate ≤ 10%; otherwise FAIL
     (exit 1) — both write the report. Exit 2 means refused (binary missing,
     a probe call broke contract, nothing was ever judged, or the scenario
     set fell below its minimums); exit 3 means judge failures exceeded 10%
     of judged items — that run measured the plumbing, not the policy, and
     nothing is written.
3. **`HU_PROSPECTIVE=shadow` for 7 days**, then
   `python3 scripts/prospective_shadow_report.py --since <day1> --until <day8>`.
   Reads the dated service log and `memory.db` read-only; reports
   `would_fire_per_day`, `resolved_before_cue_rate`, `cue_to_fire_rate`,
   `uptake`, `miss_rate` and `judge_failure_rate` for keyword cues, and the
   `time_*` equivalents for time cues, plus `write_err`. `duplicate_rate` is
   always `null` with a machine-readable reason — SHADOW never writes state,
   so a still-pending intention would-fires again on every later pass, which
   is structural and not a signal about the judge (ruling F5). Writes
   `~/.human/logs/prospective-shadow-<until>.json` (0600) on success (exit
   0); refuses (exit 2, nothing written) if the window holds no SHADOW log
   line, the database is missing or unmigrated, or any shadow log line in
   the window fails to parse — a drifted parser is worse than a partial one,
   so one bad line refuses the whole report.
4. **Spot-check 30 would-fires blind.**
   - Build the sheet:
     `python3 scripts/prospective_spot_check.py build --since <day1> --until <day8> --out-dir ~/.human/prospective-spot-check-<date>`.
     Takes the most recent 30 SHADOW `fire` verdicts plus up to 30 held items
     (`already_resolved` / `not_now` / `cancel`), redacts names/contacts,
     shuffles with a seeded RNG and renumbers ids after the shuffle so
     nothing about row order or id leaks which class a row belongs to.
     Writes `rating_sheet.csv`, a private `answer_key.json` (seed and
     fire/hold split, never opened while rating), and a `README.md` with the
     question and the scoring command. Refuses (exit 2, nothing written) if
     `--out-dir` exists, is a symlink, resolves inside this git repo tree,
     the log/database is missing, the window has a malformed log line, or
     fewer than 30 would-fires survive dedup.
   - Answer the sheet yes/no per row without opening `answer_key.json`.
   - Score it: `python3 scripts/prospective_spot_check.py score --out-dir …`.
     Precision = yes / answered among the would-fire rows; PASS (exit 0)
     needs precision ≥ 0.8 on ≥ 30 answered would-fires, otherwise exit 2
     (the result prints first either way). An answer that is not exactly
     `yes`/`no` counts as unanswered, never as `no`.
   - Set `HU_PROSPECTIVE=live` only if the step-2 harness PASSed **and**
     this precision bar is met.
5. **Then `HU_PROSPECTIVE_TIME=shadow` for 7 days**, and follow the same
   shadow-report → spot-check → precision-bar pattern before setting it to
   `live`.

Rollback at any step: unset the gate (or set it to `off`) in the launchd
plist via `scripts/install-human-daemon.sh` — never `cp` over the running
binary or hand-edit the plist — then verify with `scripts/verify-deploy.sh`.
Off is exactly the pre-v2 path.

## Before `HU_PROSPECTIVE_TIME=shadow` (rollout step 5) or `=live`: known controller gaps

A checklist for the person promoting the time gate. Items 1 and 3 are
closed (commit `9ad82174f`); items 2, 4 and 5 are documented gaps in the
current code, still open:

1. **Closed (`9ad82174f`): the history load no longer happens every tick for
   contacts with nothing due.** `pm_time_v2`
   (`src/daemon/daemon_prospective_time.c`) now runs
   `hu_prospective_repo_count_due` (open time rows with `0 < due_at <= now`)
   first and skips the whole pass, chat.db history read included, when it is
   0 — in SHADOW and LIVE. A failed count falls through to the full pass.
   Contacts with a due row load history and log exactly as before. Residual:
   in LIVE, a contact whose only due intention is memoized `not_now` (item 3)
   still loads history on each tick, though nothing is judged.
2. **The legacy `hu_superhuman_delayed_followup_mark_sent` does not retire
   the v2 twin.** A twinned follow-up (F31 legacy + its v2 mirror) can
   surface once from each side. Retire the twin on legacy mark-sent, or gate
   F31 for items that have a v2 twin.
3. **Closed (`9ad82174f`): a `not_now` verdict is no longer re-judged every
   tick in LIVE.** An in-process memo keyed by (contact, action) and the
   local day (`hu_prospective_local_day_start`) skips the judge for an
   intention already answered `not_now` today; it stays pending, is counted in
   `memo_skipped`, and does not use up the per-pass judge cap. The table is
   fixed at 128 direct-mapped slots; a colliding intention overwrites the slot,
   so the evicted one is judged once more that day. A restart forgets it.
   Parse failures and judge errors are not memoized. SHADOW does not use it.
4. **When two frames of one topic collapse to one row, settling retires only
   the first frame's ledger row.** Also retire the contact's unsent
   follow-ups whose frame topic equals the action, so the second frame's row
   does not resurface the same topic later.
5. **Backfill rows imported as `expired` do not retire their ledger twins**
   — `agent_turn.c` and `proactive.c` read commitments/follow-ups regardless
   of the gate. Retire the ledger twin at backfill time and update the
   backfill's second-run (idempotency) counts accordingly.

Ruling F16 (applies before any promotion, not just this one): the nightly
eval (`scripts/eval_prospective_memory.py`) must read v2's `status` /
`outcome` columns, not the legacy `fired` flag, before a promotion decision
is made from its numbers.

## The probe

`human prospective probe` runs the daemon's own functions
(`hu_prospective_v2_run` / `hu_prospective_v2_after_delivery`) against a
fixture database named by `--db`, which is required — it never opens
`~/.human/memory.db` by default.

```bash
D=$(mktemp -d)
build/human prospective init --db "$D/pm.db"
build/human prospective probe --full --db "$D/pm.db" --contact +15550100001 --now 1790000000 \
  --inbound "the taco place was packed" --history "$D/history.txt" --judge model
build/human prospective probe --db "$D/pm.db" --contact +15550100001 --deliver "how was the taco place?"
```

`--judge` takes `fire` / `already_resolved` / `cancel` / `not_now` (a fixed
scripted answer) or `model` (the configured provider and default model,
GLM on :8741 in prod, through the same thinking-off adapter the daemon
uses). `--tick` runs the time-cue path instead of `--inbound`.

Output contract: a `candidates=… fire=… … bytes=… write_err=…` header, then
with `--full` one `item id=… verdict=…` line per judged intention and the
directive text. `--deliver` prints `surfaced=… used=… ignored=… expired=…`.
`human prospective backfill --db PATH [--write] [--now EPOCH]` runs the same
backfill the wrapper script drives, emitting one JSON line whose last two
fields are `skipped_unsafe` (a contact-owned promise the rephraser refused to
mirror in third person is skipped, never written first-person) and
`ledger_retired` (ledger rows of expired imports retired in the same
transaction).

## Known limits

- SHADOW calls the model on cued turns, adding latency before those
  replies. The cost is bounded at 3 judge calls of at most 16 tokens each.
- A reply sent as a voice memo or a tapback never reaches the
  delivered-text hook. Its surfaced intention counts as an unused attempt
  and retries once before expiring.
- Time cues are only considered on ticks where the contact already passed
  the proactive check-in gate. `time_miss_rate` in the shadow report
  measures what that misses.
- An action whose content words are all short (<4 chars) or non-ASCII has no
  key terms (`hu_prospective_key_terms`). It can never be proven done by
  `hu_prospective_reply_uses_action`, so it always expires after two
  attempts.
- `duplicate_rate` (keyword and time) is always `null` in the shadow report
  — SHADOW's read-only pass cannot distinguish a genuinely duplicated fire
  from the same still-pending intention being re-judged every day it stays
  pending (ruling F5).
