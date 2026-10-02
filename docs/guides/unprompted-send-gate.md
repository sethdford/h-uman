---
title: Unprompted send gate — one guard stack for every message h-uman starts
created: 2026-10-02
status: operator-facing
---

# Unprompted send gate

Every message h-uman sends without being asked passes one gate stack,
`hu_unprompted_send_check` (`include/human/daemon/unprompted_gate.h`). The
unprompted paths are:

| kind | path |
|---|---|
| `proactive` | proactive proposer (`hu_service_run_proactive_checkins`), and the follow-up watcher via `hu_daemon_proactive_gate_and_send` |
| `cron` | an agent cron job aimed at a contact (`channel:contact`), e.g. from the schedule tool |
| `bump` | the read-no-reply bump (`daemon_followup_sched.c`), screened before compose and again at delivery |
| `f25` | the F25 emotional check-in |
| `photo` | the proactive photo share |
| `date_note` | an owner-approved date note (`daemon_person_dates.c`), on its way to the contact |

**Not gated: owner-scheduled messages** (`human schedule add`, `kind 0` in
`scheduled.json`). They are explicit owner intent, keep their historical
pipeline (validator chain plus sanitizer), and are delivered *before* the
daemon's global budget gate, so unprompted sends can never stall them.

The per-contact `0 10 * * *` check-in cron is **no longer registered**. Check-ins
are owned by the proactive proposer. A persona that still sets
`contacts[].proactive_schedule` gets one startup log line saying the key is
ignored.

Contacts are keyed canonically: a key that names a persona contact (by
`contact_id`, then email, then name) maps to its `contact_id`. A reply or an
opt-out sent from a contact's email address therefore counts for that contact.

## Stages (first deny wins)

1. **opt-out**: the contact asked us to stop (`contact_suppressions`).
2. **governor**: the global daily and weekly ceiling, then this contact's
   unanswered cool-off. Two unanswered means 144 h, three means 288 h, and four
   or more means never. Only an inbound from **this** contact resets it.
3. **throttle**: the per-contact cap of 1 per 24 h and 3 per 7 d, counted from
   the `proactive_decisions` ledger, so a restart cannot reset it. At send time
   the channel token bucket also applies.
4. **quiet hours**: a static sleep floor (no unprompted send from 23:00 to 06:00),
   plus the operator's autoresponder DND window. Both use the **owner's**
   timezone, the machine the daemon runs on, not the contact's.
5. **circuit breaker**: delivery to this contact keeps failing.
6. **reachability**: the `HU_PROACTIVE_REACHABILITY` pre-filter (it has its own gate).
7. **sanitizer**: at send time, when there is text. This is the PROACTIVE
   outbound pipeline, whose moderation stage blocks violence, hate and sexual
   content. It deliberately lets a self-harm mention through (checking on a
   struggling friend), so there is no second moderation pass.

Before any LLM call, a path runs stages 1–6 with `at_send=false`. Just before
`vtable->send`, it runs all stages with `at_send=true`.

**Without SQLite there are no unprompted sends.** The stack fails closed
(`cap_unverifiable`) when it has no ledger to count the cap from, so a build
with `HU_ENABLE_SQLITE=OFF` or a non-SQLite memory backend sends no proactive
check-ins, bumps, F25 check-ins, photos, contact-directed cron messages or date
notes. Owner-scheduled messages are unaffected.

These limits are **static by policy**. They are anti-spam, consent and sleep
ceilings. Adaptive cadence may only sit underneath them. The stack is ungated
because it is a correctness and safety fix, not a behaviour rollout.

## Reading it

One aggregate line per decision, with no text, names or handles:

```
[unprompted] kind=<proactive|cron|bump|f25|photo> result=<allow|deny> reason=<r> stage=<pre|send>
```

```bash
grep '\[unprompted\]' ~/.human/logs/service-loop-error.log \
  | sed -E 's/.*kind=([a-z0-9]+) result=([a-z]+) reason=([a-z_]+).*/\1 \2 \3/' | sort | uniq -c
```

Delivered sends land in `proactive_decisions` with `sent=1` and triggers
`proactive_send`, `unprompted_cron`, `unprompted_bump`, `unprompted_f25`,
`unprompted_photo` or `unprompted_date_note`. A denied date note is reported to
the owner as `couldn't send your note to <name> (<reason>)`. Each contact's last inbound is recorded in
`unprompted_contact_state`. The `'*'` row there is the ledger epoch: sends from
before it never count as unanswered.

## What should change after deploy

- `send_cap` rejections **after** an LLM approval go to 0. The cap is checked
  before the proposer (47 such rejections in 13 days before).
- `kind=bump` and `kind=f25` deny lines appear for opted-out contacts, contacts
  over the cap, and contacts in quiet hours.
- No `cron send` delivery to a contact without a matching `kind=cron result=allow`.

## Rollback

Revert the PR, then reinstall with `scripts/install-human-daemon.sh`. The new
`unprompted_contact_state` table and the `unprompted_*` rows are inert to older
binaries.
