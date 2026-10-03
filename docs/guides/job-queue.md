---
title: Durable job queue (HU_JOB_QUEUE)
created: 2026-10-03
status: operator-facing
---

# Durable job queue

One SQLite table, `jobs` in `memory.db`, for daemon work that must survive a
restart. The first two uses are inbound messages held while the local model is
down, and scheduled contact sends. Design and plan:
[`docs/plans/2026-10-03-durable-job-queue.md`](../plans/2026-10-03-durable-job-queue.md).

- Repository: `include/human/memory/job_queue_repo.h`,
  `src/memory/repos/job_queue_repo_sqlite.c`.
- Daemon side (gate, start recovery, counters): `include/human/daemon/job_queue.h`,
  `src/daemon/daemon_job_queue.c`, called once from `hu_service_run` at startup.
- Inbound hold (below): `include/human/daemon/job_hold.h`,
  `src/daemon/daemon_job_hold.c` (decide + hold), `src/daemon/daemon_job_release.c`
  (release, cancel, expiry).

## Delivery guarantee

Contact sends are **at most once**: a missed message is better than a
duplicate. A worker claims a due job (`pending → claimed`, with a lease), and
commits `claimed → sending` before it calls the channel. If the process dies
after that commit, the next start moves the row to `unknown`, and it is never
claimed or sent again. A claim whose lease expired never reached `sending`,
so it goes back to `pending` and is retried.

Two limits on that guarantee:

- **Process crash, not power loss.** The queue shares the daemon's `memory.db`
  connection, which keeps SQLite's default sync settings. On macOS `fsync`
  does not flush the drive's write cache, so after a power cut a committed
  `sending` can revert to `claimed` and be retried. Turning on
  `PRAGMA fullfsync` would fix that, but it would slow every memory write on
  the shared connection, so it is left off. Revisit it, or give the queue its
  own connection, before scheduled sends go live.
- **No shared transactions.** Gateway worker threads use the same connection
  (opened `FULLMUTEX`). `claim_due`, `mark_sending` and `recover_on_start` run
  their own `BEGIN IMMEDIATE … COMMIT`. If another transaction is already open
  on the connection they refuse with `HU_ERR_IO_BUSY`, and log that once,
  rather than joining it. If they joined, that transaction's `ROLLBACK` could
  undo a committed `sending`. A failed `COMMIT` is returned as an error, so a
  worker never sends on a transition that did not stick. A `claimed` row can
  only be finished (`failed`, `canceled`, `expired`) by the claim that holds its
  lease.

## Gate: `HU_JOB_QUEUE=off|shadow|live`

Parsed by `hu_gate_mode_from_env`. Unset, or any value it does not recognise,
means `off`.

| Mode | At daemon start | Effect on sends and replies |
|---|---|---|
| `off` (default) | One log line naming the mode. No table, no query. | None |
| `shadow` | Creates `jobs` if missing, runs start recovery, logs one count line | None |
| `live` | Same as `shadow` | None yet: no producer or consumer uses the queue |

The two start lines look like this. They carry counts only, never message text,
contacts or keys:

```
[jobq] durable job queue HU_JOB_QUEUE=shadow
[jobq shadow] start: recovered unknown=0 requeued=0 | pending=0 claimed=0 sending=0 unknown=0 done=0 failed=0 expired=0 canceled=0 shadow=0
```

A later PR moves scheduled sends onto the queue, behind its own shadow logging.

## Inbound hold: `HU_JOB_HOLD=off|shadow|live`

The nightly retrain boots `mlx-server` out for about 70 minutes. With
`privacy.local_only` there is no cloud fallback, and the iMessage poll has
already saved its cursor past the message, so a reply turn that fails in that
window is a message nobody ever answers. The hold path keeps those messages and
hands them back to the normal reply turn when the model is back.

**When a failed turn is held.** `hu_job_hold_decide` returns HOLD only when
both hold:

- the turn failed with a transport error (`hu_agent_error_is_transport`:
  `HU_ERR_IO`, `HU_ERR_TIMEOUT`, `HU_ERR_PROVIDER_UNAVAILABLE`), and
- `hu_mlx_admin_probe_health` against the `mlx_local` provider's base URL says
  the server is down (the probe caches its answer for 60 s).

A transport error while the probe is up, or with no `mlx_local` provider
configured, is a one-off: today's behaviour (no reply), counted and logged as
`[jobq] one-off err=<code> probe=up|unknown`. Any other error is ignored by the
hold path. Only the iMessage channel is held.

The hook is `hu_daemon_jobs_on_turn_error`, which replaced the failed-turn log
in `hu_service_run` and still prints that line unchanged. Release is
`hu_daemon_jobs_poll`, which replaced the channel poll call.

| Mode | Failed turn, model down | Poll tick (iMessage, at most every 30 s) |
|---|---|---|
| `off` (default) | Logs the failure, as before. Nothing else. | Calls the channel's poll; output untouched |
| `shadow` | `[jobq] shadow would hold n=<count> err=<code> probe=down`; remembers rowids in memory only | `[jobq] shadow would expire n=…`, `would release n=… age_max=…s`, `would cancel n=…`; batch untouched |
| `live` | One `inbound_hold` job per message, key `hold:<chat_id>:<rowid>`; `[jobq live] held n= dup= skipped=` | Expires holds older than 3 h, then, if the probe is up, claims due holds, cancels any the owner already answered, and puts the rest at the front of the poll batch; `[jobq live] released n= age_max= canceled=` |

`live` needs the jobs table, so it also needs `HU_JOB_QUEUE=shadow|live` and a
clean start. Without that it runs as `shadow` and logs why at start. Log lines
carry counts, error enums and ages only, never text, handles or keys.

**Payload.** A compact binary record of what re-injecting the message needs
(rowid, timestamp, guid, reply-to guid, chat id, flags, text); the sender handle
is the job's `contact`. If it would exceed 4096 bytes the message is not held
(`[jobq live] not held: … over the 4096-byte payload cap`) and gets today's
behaviour.

**At most once.** Release runs claim → `mark_sending` → copy into the batch →
`finish done` inside one poll call, before the turn that answers the message
runs. A crash before `mark_sending` leaves nothing sent and the lease expires,
so the row is retried. A crash after it leaves the row `sending`, which start
recovery turns into `unknown`, never re-claimed: the message may go unanswered,
never answered twice. A released message whose turn fails again is not held
again (its key already exists). The daemon's reply dedup still applies to the
re-injected batch as a second guard.

**Owner already answered.** Before release, `hu_imessage_channel_replied_after`
asks chat.db whether a human-written outbound (not one of the daemon's own sends)
landed in that conversation after the held rowid. If so the job is `canceled`.

**Expiry.** A hold older than 3 hours (the retrain window is 02:00–05:00) is
`expired` before any release is attempted, and the owner gets one Notification
Center banner per expiry batch naming only the count.

The contact never gets an "I'm down" message. They see at most a typing
indicator, then a late normal reply.

## Promotion

`off → shadow` is safe at any time. In this release it only creates an empty
table and runs a recovery that has nothing to recover.

`shadow → live` for the features built on the queue is gated on the
measurement in §9 of the design:

- **Inbound hold (`HU_JOB_HOLD` shadow → live):** at least 7 shadow nights
  where:
  - `would hold` covers 100% of the turns that failed with a transport error
    between 03:07 and 04:20. Count both sides from the service log:
    `agent turn failed for … (I/O error|timeout|provider unavailable)` lines
    against the `n=` sum of `[jobq] shadow would hold` lines in that window.
  - there are 0 would-holds while the model probe is healthy. Every transport
    failure outside the outage must log `[jobq] one-off … probe=up`, never
    `would hold`.
  - the first `[jobq] shadow would release` line comes within 5 minutes of
    mlx-server reporting healthy (`age_max` minus the outage length).
  - `would expire` stays at 0 on normal nights.

  After live, held-reply delivery should be at least 95%. Measure it as the sum
  of `released n=` over the sum of `held n=`, with `canceled` and `expired`
  reported alongside.
- **Scheduled sends:** 14 days of shadow with no disagreement between the
  legacy queue and the job queue beyond the known replayed and dropped
  classes, plus one manual `kill -9` drill between `sending` and `done` that
  shows no re-send.

## Rollback

Remove `HU_JOB_QUEUE` from the launchd plist environment, or set it to `off`,
and restart the daemon. To turn off only the inbound hold, remove
`HU_JOB_HOLD` (or set it to `off`) and restart. Rows already held stay in the
table and are not released, so they go unanswered, just as they would have
without the hold. `live → shadow` stops writes and releases on the next start. The `jobs` table can stay where it is: in `off` mode
nothing reads or writes it.
