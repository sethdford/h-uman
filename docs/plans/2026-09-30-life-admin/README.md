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
| 1 | **Reminders**: "remind me to X at/on/in …" from the owner chat; durable store (memory repo), claim/missed semantics, delivery to the owner chat, "done"/"snooze" replies | `HU_REMINDERS` OFF → SHADOW → LIVE | a week of shadow log on the owner's real messages with no ordinary chat misread as a command. Shadow stores nothing, so exactly-once delivery is pinned by tests, not measured here |
| 2 | **Morning briefing** to the owner: today's calendar, due reminders, commitments due, dates in the next 7 days, weather — one message, skipped when empty | `HU_BRIEFING` | owner rates a week of shadow briefings |
| 3 | **Commitments, both directions**, surfaced to the owner with their source | reuse `HU_PROMISE_KEEPER` | precision on a labelled sample of detected commitments |
| 4 | **Important dates → drafted message** for the owner to approve (never auto-sent) | `HU_DATE_NUDGES` | owner approval rate of drafts |
| 5 | **Calendar**: fix the installed-daemon script path, feed busy blocks into delivery timing, 15-minute meeting prep | `calendar_enabled` | briefing/meeting-prep accuracy vs Calendar.app |

Contact-facing reach-outs keep their existing gate; if and when they are
enabled, adopt Ferni's backoff (1/2/4/7 days, stop after 3 unanswered) in the
governor, measured by the blind A/B, not by tests.

## Slice 1 status (2026-09-30)

Built and gated OFF: `src/daemon/daemon_reminders.c`,
`src/memory/repos/reminder_repo_sqlite.c`, hooked into the owner-DM path of
`src/daemon.c` next to saved shares, with a tick in the service loop.

- Phrasings: "remind me to X at 5pm / in 20m / tomorrow / on friday / tonight",
  time first ("remind me tomorrow at 9 to X"), "remind me to X" then a bare
  time as the next message, "reminders", "done", "snooze [30m]",
  "remind me again in 20 min", "remind me later".
- A time phrase is accepted only as a whole run of time words at the start or
  end, so time words inside the task stay in the task.
- "at 7" with no am/pm means the next 7 today; on another day 1–6 means
  afternoon, 7–11 morning.
- Delivery is claim-based (at most once per claim, a crash mid-send retried
  after 10 minutes), only ever to the owner (re-checked at send), and a
  reminder more than 2 hours late is mentioned once in a single "missed
  these" message rather than sent as if on time.

Known limits, deliberately left for later: calendar dates ("Oct 12", "the
12th") are not parsed — the reminder falls back to asking "when?"; the
pending "when?" slot holds one task; recurring reminders are not supported.

Not reused: the `prospective_tasks` table created in
`src/memory/engines/sqlite.c` and its `hu_prospective_task_t` struct have no
readers or writers anywhere (checked 2026-09-30), and lack the owner,
channel and status columns delivery needs. They are left for a dead-code pass
rather than removed in this feature change.

## Slice 2 status (2026-09-30)

Built and gated OFF: `src/daemon/daemon_briefing.c`,
`src/memory/repos/briefing_repo_sqlite.c`, ticked from the service loop next
to reminders.

- One message: today's calendar (when `context_awareness.calendar_enabled`),
  reminders due today, commitments due (yours and theirs, each quoting the
  words that were said, overdue ones for 7 days only), important dates in the
  next week, and weather (when `context_awareness.weather_enabled`, a
  `location`, and `HU_WEATHER_API_KEY`). No calendar, reminder, commitment or
  date means no message.
- Sent at `HU_BRIEFING_HOUR` (default 8) local, until noon; later mornings are
  skipped, not sent late. The day is claimed durably before sending and
  released only when the send fails.
- Shadow writes `<state dir>/briefings/<day>.txt`, which is what the owner
  rates for the go-live measurement.
- Temperature is shown in °F because the owner is in the US; a units setting
  waits for an owner who needs one.

Found for slice 5: `scripts/calendar_query.applescript` does not escape event
names, so one title containing a quote makes the whole day's JSON unparseable
(the briefing logs it and leaves the calendar out), and its start times are
locale-dependent date strings; emitting hour/minute numbers would remove the
guesswork.

## Slice 5 status, first half (2026-09-30)

Done in `src/platform/calendar_macos.c`:

- The AppleScript is embedded in the binary and run as `osascript -e …`
  lines, so the installed daemon no longer depends on a `scripts/` directory
  it never had; `scripts/calendar_query.applescript` is gone.
- Titles are JSON-escaped in the script (a quote, backslash or newline in a
  title no longer invalidates the day), each event carries numeric `h`/`m`
  (no locale-dependent date parsing), and all-day events in progress are
  included and marked `allday`.
- The query is bounded to 20 s, so a pending Calendar permission prompt
  cannot stall the service loop, and a failure is logged once instead of
  looking exactly like an empty calendar.

Verified without touching the owner's calendar: `osacompile` compiles the
exact embedded lines, the escape handler round-trips a hostile title through
a real JSON parser under `osascript`, and all 38 lines appear verbatim in the
built binary.

Still open: calendar busy blocks deferring reminder/briefing delivery, and
15-minute meeting prep. Both need the owner's real calendar to measure.
