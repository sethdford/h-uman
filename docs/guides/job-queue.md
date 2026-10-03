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

## Delivery guarantee

Contact sends are **at most once**: a missed message is better than a
duplicate. A worker claims a due job (`pending → claimed`, with a lease), and
commits `claimed → sending` before it calls the channel. If the process dies
after that commit, the next start moves the row to `unknown`, and it is never
claimed or sent again. A claim whose lease expired never reached `sending`,
so it goes back to `pending` and is retried.

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

Later PRs add the inbound hold path and move scheduled sends onto the queue.
Each of those changes what is sent, and each has its own shadow logging.

## Promotion

`off → shadow` is safe at any time. In this release it only creates an empty
table and runs a recovery that has nothing to recover.

`shadow → live` for the features built on the queue is gated on the
measurement in §9 of the design:

- **Inbound hold:** at least 7 shadow nights where `would hold` covers 100% of
  the turns that failed with a transport error between 03:07 and 04:20, with
  0 would-holds while the model probe is healthy, and releases within 5 minutes
  of mlx-server reporting healthy.
- **Scheduled sends:** 14 days of shadow with no disagreement between the
  legacy queue and the job queue beyond the known replayed and dropped
  classes, plus one manual `kill -9` drill between `sending` and `done` that
  shows no re-send.

## Rollback

Remove `HU_JOB_QUEUE` from the launchd plist environment, or set it to `off`,
and restart the daemon. The `jobs` table can stay where it is: in `off` mode
nothing reads or writes it.
