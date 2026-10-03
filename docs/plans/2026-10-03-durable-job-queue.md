# Design: durable SQLite job queue in the h-uman daemon (v1)
Base: main bd3350681. Chosen over Temporal: an in-process durable queue behind a memory repository.

## 1. Problem (evidence)
P1 inbound lost while the model is down. scripts/nightly-retrain.sh:422-467 boots out ai.human.mlx-server (back 04:16:37 on 10-02). Local→cloud fallback (src/daemon.c:6691) is skipped when hu_local_only_enforced() — live config has privacy.local_only=true — so a failed turn only logs (daemon.c:6780-6783) + stop_typing (:6893-6897); nothing sent. The chat.db cursor is persisted at poll time (src/channels/imessage.c:6573,6591), before the agent turn, so the message is never re-seen (reply_dedup.h:14-24 comment saying otherwise is stale). UNCONFIRMED: no log line showing an actual inbound inside the retrain window.
P2 scheduled sends use divergent mechanisms: in-memory 16-slot queue (conversation.h:1020, conversation.c:8593) mirrored to scheduled.json, text cut at 511 B, fed by follow-ups (daemon_followup_sched.c:185), `human schedule add` (app/main.c:491) and spontaneity; follow-up dedupe is a static in-memory store (daemon_followup_sched.c:51-52); cron/check-ins fire once on minute match, no retry (daemon_cron.c:246,294); reminders are at-least-once (reminder_repo.h:6-12); prospective memory only surfaces in the prompt (out of scope).
P3 crash/failure mid-send: flush clears the slot in memory (conversation.c:8711), sends (daemon_unprompted_sends.c:272), then persists (:318) — a crash between leaves the slot on disk, reloaded (:302) and re-sent (duplicate; inferred). A failed send returns true (:275) so persist deletes it (lost; comment daemon_followup_sched.c:200-203). A deferred send returns false (:229-233) after the slot was cleared in memory — vanishes until restart, then fires late.

## 2. Goals / non-goals
Goals: one durable queue; at-most-once for contact sends (owner ruling: missed > duplicate); hold inbound while the model is down; migrate exactly ONE mechanism. Non-goals v1: replacing cron, reminders, prospective, share queue, check-ins; cross-process workers; re-sending unknown-status deliveries.

## 3. Schema
include/human/memory/job_queue_repo.h + src/memory/repos/job_queue_repo_sqlite.c (free-function shape like outbound_sends_repo.h; sqlite3 only in repos/).
jobs(id INTEGER PK, kind TEXT CHECK(kind IN('inbound_hold','sched_send')), payload BLOB, contact TEXT, channel TEXT, due_at INT, created_at INT, state TEXT CHECK(state IN('pending','claimed','sending','done','failed','unknown','expired','canceled','shadow')), attempts INT DEFAULT 0, lease_until INT, idempotency_key TEXT UNIQUE NOT NULL, last_error TEXT, updated_at INT); INDEX(state,due_at).
Repo API: ensure_schema, enqueue (INSERT OR IGNORE on key), claim_due, mark_sending, finish, release, recover_on_start, expire_older_than, counts.

## 4. Claim / lease (at-most-once)
1 claim: one txn pending→claimed, lease_until=now+120s. 2 gates (unprompted, opt-out); a deferral releases back to pending, no attempt counted. 3 mark_sending: commit state=sending, attempts++ BEFORE vtable->send. 4 finish: success→done; explicit not-delivered (e.g. blue_guard HOLD)→failed or requeue. 5 recover_on_start: sending→unknown (never re-claimed; counted; owner notified); claimed with expired lease→pending.
Idempotency keys: sched:<sha1(contact|due_at|text)>; followup:<contact>:<msg_id> (replaces in-memory dedupe); hold:<chat_id>:<rowid>.

## 5. Inbound hold while the model is down
Hold only when BOTH: transport error (IO/TIMEOUT/PROVIDER_UNAVAILABLE per agent.c:121-135, expose a public predicate — agent_internal.h is private) AND hu_mlx_admin_probe_health reports down (60 s cache, model_router_health.h:16). Transport error with healthy probe = one-off: today's behaviour, counted.
Hold each msgs[batch_start..batch_end] (hu_channel_loop_msg_t carries timestamp_sec, chat_id) as an inbound_hold job. Release: a poll wrapper re-checks the probe; when healthy, appends due held messages after poll_fn so they take the normal turn (hu_daemon_reply_dedup_already_replied still blocks dupes); cancel if hu_imessage_user_replied_after (imessage.c:106) shows Seth already answered. Max hold age 3 h (HU_TRAIN_WINDOW=02:00-05:00; observed outage ~70 min) → expired + one owner notification. Contact sees at most a typing indicator then a late normal reply — never an "I'm down" message (AI tell).

## 6. Migration + gate
v1 migrates the scheduled-send queue (all three P3 bugs; three producers). Gate HU_JOB_QUEUE=off|shadow|live (hu_gate_mode_from_env), one startup log line.
Shadow: each delivery pass mirrors active hu_conversation_sched_slot(i) entries into jobs (state='shadow'), no producer changes; logs `[jobq] shadow sched: legacy=sent|deferred|dropped|replayed jobq=would-send|would-requeue|would-refuse`; holds log `would hold #xxxx err=… probe=down`, `would release n age=…`, `would expire`. Never text or handles.
Live: producers enqueue directly; delivery claims from jobs; `human schedule` reads/writes jobs; scheduled.json imported once then renamed .migrated.

## 7. Tests
Repo on :memory:: exclusive claim, lease expiry, sending→unknown never re-claimed, duplicate key, age expiry. Pure predicate: transport+probe down=HOLD, probe up=ONE_OFF, non-transport=ONE_OFF. Crash injection: stop after mark_sending, recover, mock channel send NOT called. Release: owner-replied cancel; expiry ordering. Source-gate symmetry with HU_ENABLE_SQLITE.

## 8. Ratchets
src/daemon.c 9659 = ceiling; hu_service_run at its function ceiling. Only two daemon.c hooks, each net zero lines: poll_fn (:2390) → hu_daemon_jobs_poll(...); the 4-line error log (:6780) → hu_daemon_jobs_on_turn_error(...). Logic in src/daemon/daemon_job_queue.c and daemon_job_release.c (<800 LOC each). sqlite includers stay ≤87.

## 9. Promotion measurement
Hold → live: ≥7 shadow nights with would-hold covering 100% of transport-failed turns 03:07–04:20, 0 would-holds while probe healthy, releases within 5 min of mlx-server healthy. Sched → live: 14 days shadow, no disagreement except known replayed/dropped classes, plus one manual kill -9 drill between sending and done showing no re-send. After live: unknown ≈0/week; held-reply delivery ≥95%.

## 10. PR plan (~1,400 prod + ~900 test LOC; each PR ≤250 prod LOC)
1 repo header + sqlite impl + tests (~240). 2 daemon_job_queue.c: gate, recovery at start, startup line, metrics (~180). 3 model-down classifier + hold hook, shadow (~200). 4 release wrapper, owner-replied cancel, expiry + owner notify (~230). 5 sched-send shadow mirror + agree/disagree logging (~200). 6 live sched claim path, CLI, scheduled.json import (~240). 7 doctor check, docs, follow-up dedupe → idempotency keys (~120).
