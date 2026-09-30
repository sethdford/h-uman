# Life admin: reminders, briefings, commitments, dates, calendar

Written 2026-09-30 against `origin/main` `a7449f48b`. Asked for: "calendars,
reachouts, reminders … in a natural, better-than-human way that feels
seamless". This plan decides what to build, in what order, and what stays
gated. Every "state" below was verified in code, not taken from a survey.

## What we learned from Ferni (voiceai)

Ferni has all of these features and very few of them reach a user: reminders
were delivered 0/123 in prod (in-process timers that only saw their own
memory), outreach runs five overlapping pipelines whose main one records a
decision and never sends, web push cannot work (the library is not
installed), and the morning briefing's weather/calendar fields are declared
but never filled. The parts worth copying are designs, not code:

- **Consent by default, escalation by opt-in** (#111): in-app/owner-facing is
  the default; anything that reaches someone else is opt-in.
- **Cadence that backs off**: 1/2/4/7 days by silence, stop after 3
  unanswered, reset on reply; quiet hours in the recipient's time zone.
- **Durable, exactly-once delivery**: claim each due item in a transaction
  (pending → sending → sent), mark items more than 2 h late `missed` instead
  of sending stale nags, never report `delivered` without a real send.

## What h-uman already has (verified 2026-09-30)

| Area | State |
|---|---|
| Owner ↔ daemon channel | **Live.** Your own number is a persona contact with `relationship:"test"`; the daemon already takes `#voice`/`#share` self-tests and "for mom: <link>" share commands from it (`hu_share_is_owner`). |
| Contact check-ins, emotional check-ins, bookends, photo shares | Gated: `proactive.master_enabled` (default off) + a governor (6/day, 15/week, cool-off after 2 unanswered). |
| Scheduled messages (`human schedule add`) | Gated: only flushed inside the contact check-in pass. |
| Their commitments (F20), dated follow-ups, promise keeper, follow-up watcher | Stored or computed; surfacing gated off or shadow. |
| Birthdays / important dates | Reply context live; proactive nudge gated. |
| Prospective memory | Cue-triggered reminders render in replies (live). `prospective_tasks` (time-triggered) has a table and a struct but no functions. |
| macOS Calendar context | Gated (`calendar_enabled`), and the AppleScript is located relative to the build tree, so the installed daemon likely finds none. |
| Weather, feeds (incl. Gmail) | Weather needs a key + location; feeds ingest live but the digest never reaches you. |

## Bugs found while verifying (fix first)

1. **Initiative kill switch leaks an LLM call.** `hu_init_proposer_tick`
   returns `SKIP` both for "disabled" and for "all gates passed"; the wrapper
   treats `SKIP` as "go", and the daemon calls it every loop with no outer
   gate. With `initiative.enabled=false` (the default) every loop iteration
   builds a context bundle and calls the model. Latent on the owner's machine
   (initiative is on there: 81 "activated", 0 "disabled" in the log).
2. **Task tools can never succeed.** `task_create/update/list/get` are
   registered with a NULL task manager; every call returns an error.
3. **Calendar `delete` reports success without deleting.** It issues a GET and
   returns `{"deleted":true}`. `availability` returns "unsupported" in prod.
4. **The good-morning scheduler is unreachable.** It requires 06–08 local, but
   its caller returns outside 09–21.

## Principles for "better than human"

- **Owner first.** Everything in this plan is *for* the owner and is
  delivered *to* the owner. Nothing here texts a contact as the owner; the
  contact-facing check-ins stay behind their existing gate and blind-A/B
  measurement (`.claude/rules/feature-gate-requires-measurement.md`).
- **One voice, few pings.** A human assistant does not send six
  notifications; they send one well-timed message. Items due in the same
  window are merged into one message in the persona's voice, and a day with
  nothing worth saying sends nothing.
- **Never forget, never nag.** Durable storage and exactly-once delivery;
  late items are marked missed and mentioned once, not re-sent.
- **Know when not to talk.** Calendar busy blocks and quiet hours defer
  delivery; the owner's own recent activity counts as "awake".
- **Say where it came from.** A nudge names its source ("you told Dana on
  Tuesday you'd send the deck") so it is checkable, never invented.

## Slices (one PR each, in order)

| # | Slice | Gate | Measurement to go live |
|---|---|---|---|
| 0 | Fix the four bugs above | none (bug fixes) | tests pin each bug |
| 1 | **Reminders**: "remind me to X at/on/in …" from the owner chat; durable store (memory repo), claim/missed semantics, delivery to the owner chat, "done"/"snooze" replies | `HU_REMINDERS` OFF → SHADOW → LIVE | owner-visible shadow log for a week: every due reminder would have fired once, none duplicated, none stale |
| 2 | **Morning briefing** to the owner: today's calendar, due reminders, commitments due, dates in the next 7 days, weather — one message, skipped when empty | `HU_BRIEFING` | owner rates a week of shadow briefings |
| 3 | **Commitments, both directions**, surfaced to the owner with their source | reuse `HU_PROMISE_KEEPER` | precision on a labelled sample of detected commitments |
| 4 | **Important dates → drafted message** for the owner to approve (never auto-sent) | `HU_DATE_NUDGES` | owner approval rate of drafts |
| 5 | **Calendar**: fix the installed-daemon script path, feed busy blocks into delivery timing, 15-minute meeting prep | `calendar_enabled` | briefing/meeting-prep accuracy vs Calendar.app |

Contact-facing reach-outs keep their existing gate; if and when they are
enabled, adopt Ferni's backoff (1/2/4/7 days, stop after 3 unanswered) in the
governor, measured by the blind A/B, not by tests.
