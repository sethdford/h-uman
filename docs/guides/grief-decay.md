---
title: Grief decay — HU_GRIEF_DECAY gate for check-ins after a hard moment
created: 2026-10-02
status: operator-facing
---

# Grief decay (`HU_GRIEF_DECAY`)

When a contact's **last** inbound message classified heavy or grief
(`hu_proactive_should_suppress_for_emotion`, `src/humanness.c`), every
proactive check-in to them was suppressed, with no time limit. One sad
message silenced that contact for as long as it stayed their last message.
Prod, 2026-09-19 → 10-02: 153 suppressions, all one contact, about 12 a day.
A person does the opposite: a short pause, then a gentle "how are you
holding up".

`src/daemon/daemon_grief_decay.c` time-decays the suppression. The call site
is the P6-3 gate in `hu_service_run_proactive_checkins` (`src/daemon.c`).

## Semantics

| Mode | Effect |
|---|---|
| `off` (default) | Byte-identical to before: a heavy/grief last inbound suppresses. |
| `shadow` | Same decision. Each suppression logs `[grief_decay shadow] heavy=1 age_h=<hours since the message> quiet_h=<window> would_allow=<0/1>`. |
| `live` | Suppressed only inside the quiet window after the heavy message. After it the check-in is **eligible**: it still needs every other gate (24 h since the last message either way, governor, throttle, reachability, opt-out, the proposer's own judgement). |

LIVE requires `HU_PROPOSER_CONTEXT=live` with a local provider, because a
gentle check-in needs the proposer to see the thread. The proposer block
then carries one extra line, `Their last message was emotionally heavy, about
N hours ago.` (a number, never the text). Without that context the old
suppression holds and the line says `grief_decay=blocked_no_context`. On a
tick where a check-in goes out after a heavy message, the contact gets no
bookend greeting and no important-date extra (birthday confetti).

Each decision logs `[grief_decay live|shadow] heavy=1 age_h=… quiet_h=…
would_allow=0|1 context=0|1 grief_decay=quiet|eligible|blocked_no_context`.
A timestamp that does not parse fails closed (the old suppression). Logs
carry hours and the decision only, never text or contact ids.

**Gate order.** This gate only decides whether the contact is *eligible* this
tick. Everything after it in `hu_service_run_proactive_checkins` still
applies, including the unprompted-send gate stack (#597, not yet merged at
the time of writing); when #597 lands, its gate runs at send time, after this
one, so a GENTLE verdict can still be stopped there.

## The window

`HU_GRIEF_DECAY_QUIET_HOURS`, default **24 h** ("the next day"), clamped to
1 h – 14 days. It is a documented default, not a measurement: the emotion
card measures Seth's *reply* to distress (`distress_reply`, n = 11), not how
long he waits before following up. The learned value belongs to the v2
learned-behaviour profile (static-rules inventory §5.2, follow-up group:
`followup_delay_p50[heavy]` per contact); the TODO is in
`include/human/daemon/grief_decay.h`. Note the general rule that a check-in
needs 24 h since the last message in either direction, so a window shorter
than 24 h only matters on the feed-match path.

## Promotion: SHADOW → LIVE

1. Run `shadow` for 7 days. `grep 'grief_decay shadow' ~/.human/logs/service-loop-error.log`
   gives the number of suppressions that would have become eligible
   (`would_allow=1`) and their ages.
2. Owner review: for each contact behind a `would_allow=1` line, Seth confirms
   that a next-day check-in fits that thread. A "no" for any contact means
   the window is too short for that kind of message; raise
   `HU_GRIEF_DECAY_QUIET_HOURS` and repeat step 1.
3. Flip to `live`. For 14 days, compare check-ins sent after a heavy message
   against all check-ins in `proactive_sends`: reply rate not lower, and zero
   hurt hand-offs (`daemon_hurt_handoff`) or opt-outs from those contacts.

## Rollback

`PlistBuddy -c 'Set :EnvironmentVariables:HU_GRIEF_DECAY off'` on
`~/Library/LaunchAgents/ai.human.service-loop.plist`, then reload the service.
