---
title: Commitment guard — HU_COMMITMENT_GUARD gate, calendar permission and promotion
created: 2026-10-02
status: operator-facing
---

# Commitment guard (`HU_COMMITMENT_GUARD`)

The twin must never commit Seth to something he can't or wouldn't do. Before a
drafted reply is sent, the guard asks whether it:

- accepts or proposes a plan or a time ("yeah saturday works", "i'll be there at 7");
- agrees to give or lend money or things;
- promises a favour; or
- makes a sensitive decision or disclosure.

Code: `src/daemon/daemon_commitment_guard.c`, `src/daemon/calendar_free_busy.c`,
`tools/calendar-free-busy/`. Contract: `include/human/daemon/commitment_guard.h`.
Call site: `src/daemon.c`, right after the reactive retry loop ends
(`hu_daemon_director_end_turn`). Everything downstream sees the guarded text:
DPO pairing, memory, shaping and the send.

## How it decides

1. **Prefilter (recall, not the decision).** A cheap keyword pass over the
   draft and the message it answers looks for:
   - time and date expressions: weekdays, "tonight", "tmrw", "7pm", "at 7", "10/3";
   - commitment verbs: "i'll", "count me in", "that works", "promise", "go ahead";
   - money: "venmo", "lend", "$40";
   - a bare yes ("yeah", "sure") to an inbound request ("can you…", "u free…",
     "lend me…").

   It only decides whether to spend one local model call. Most turns stop
   here. It leans wide on purpose: a false positive costs one call, a miss lets
   a commitment through unchecked. Shadow audits every 10th miss to measure
   what it misses (below).
2. **Local detector.** The agent's loopback provider only: the reliable
   wrapper's primary, never its cloud fallbacks. No loopback model means no
   call at all. The request carries `X-HU-Purpose: commitment_check` and no
   `X-HU-Priority`, because it is on the reply path. It returns
   `{"kind":"none|plan|money|favour|sensitive","stakes":"low|high","when":…,"confidence":…}`.
   `when` is resolved in local time: a timed commitment checks a 2 h window, a
   date-only one checks 09:00–22:00.
3. **Calendar.** For plans (and small favours with a time), the guard reads
   free/busy for that window from the EventKit helper. It gets busy intervals
   only: no titles, no attendees.
4. **Decision** (`hu_commitment_decide`, confidence floor 0.6):

| Detected | Calendar | Decision | LIVE action |
|---|---|---|---|
| plan | busy | `rewrite_conflict` | local rewrite that doesn't commit ("ah i think i've got something then, lemme check"), owner notified |
| plan | unknown (no access, no helper, no time named) | `hold` | non-committal rewrite, owner notified |
| plan / small favour | free | `allow` | none |
| money, sensitive, big favour | (not checked) | `hold` | non-committal rewrite, owner notified |
| none, or confidence < 0.6 | | `allow` | none |
| detector down | | `detect_failed` | none, except the money floor below |

**Safety floors.** These are deterministic, because a wrong send commits
Seth's real life:

- Every rewrite is re-checked by the detector. If it still commits, or the
  rewrite or the re-check fails, **nothing is sent** (`action=suppressed`) and
  the owner is notified.
- If the detector is down and the draft offers money ("venmo you", "lend you",
  "pay you"…), the guard treats it as a money commitment and holds it.

**Owner notice.** This is the existing local notification
(`hu_owner_notify_local`, a Notification Center banner). It names the contact
and the kind of commitment, never what was said. There is no approve/deny
shortcut yet, because nothing reads a reply to the banner. The notice tells
the owner to answer the thread himself.

If the banner cannot be shown (osascript fails), the guard retries once. The
reply stays held either way: a failed notice never lets the committing draft
through. A notice that still fails is counted, not silent, with one
counts-only line (no text, names or handles), and the reply line reports
`notified=0`:

```
[HU_COMMITMENT_GUARD] notify_failed count=<process total> action=<suppressed|rewritten> kind=<kind>
```

Check before and during LIVE: `grep -c 'HU_COMMITMENT_GUARD\] notify_failed'
~/.human/logs/service-loop-error.log` should be 0. Any hit means Seth was not
told about a held reply. That contact got no answer (`suppressed`) or a
non-committal one (`rewritten`), so check those threads by hand.

## Gate

| Value | Behaviour |
|---|---|
| unset / `off` | Nothing runs. The reply is byte-identical (`run_off_is_byte_identical_and_calls_nothing`, `glue_off_by_default_leaves_reply_untouched`). |
| `shadow` | Prefilter, detector and calendar run, and the reply is unchanged. One line per reply (below). Every 10th prefilter miss also runs the detector (`audit=1`) to measure prefilter recall. |
| `live` | The decision is applied. |

Shadow and live log one aggregate line per reply. It holds enums, booleans,
latencies and byte counts, never text, names or handles:

```
[HU_COMMITMENT_GUARD shadow] prefilter=1 audit=0 detector=ok kind=plan stakes=low when=1 conf=0.90 calendar=busy decision=rewrite_conflict action=none notified=0 prefilter_us=3 detect_ms=410 calendar_ms=38 rewrite_ms=0 draft_b=24 out_b=24
```

`detector=` is `skipped` (prefilter miss), `ok`, `failed` (call or parse
failed) or `unavailable` (no loopback provider). **Added latency** on the
reply path is `prefilter_us + detect_ms + calendar_ms` (+ `rewrite_ms` in
live).

## One-time setup: calendar permission

1. `scripts/install-human-daemon.sh` builds the helper into
   `~/.local/bin/hu-calendar-free-busy` (macOS only; it prints a warning and
   carries on without `swiftc`). Override the path with
   `HU_CALENDAR_HELPER=/abs/path`.
2. Check its state: `~/.local/bin/hu-calendar-free-busy --status` prints
   `{"access":"granted|denied|undetermined"}`.
3. macOS ties the Calendar grant to the **responsible process**. When the
   daemon spawns the helper, that is `human-daemon`. When you run it in
   Terminal, it is Terminal. With the gate on, the first plan the guard
   checks makes the helper ask macOS once (`--prompt`). Allow
   "human-daemon would like to access your calendar". That first turn reads
   `calendar=unknown`.
4. If no prompt appears, or you clicked Don't Allow, enable `human-daemon`
   under System Settings › Privacy & Security › Calendars. Running
   `hu-calendar-free-busy --request-access` in Terminal grants Terminal, not
   the daemon. It is useful only to confirm the helper works.
5. Denied or missing access is never an error. The guard reads
   `calendar=unknown`, which **holds** plan commitments. The shadow lines show
   it: if every plan line says `calendar=unknown`, the daemon has no access.

The prompt-from-daemon path (step 3) was not exercised while building this.
The first shadow day is what confirms it works.

## Promotion: SHADOW → LIVE

Run shadow for **at least 5 days** with the local model answering. Then:

1. **Coverage.** At least 30 lines with `kind != none` and `detector=ok`. If
   more than 20% of the `prefilter=1` lines say `detector=failed` or
   `unavailable`, the result is INCONCLUSIVE, not PASS.
2. **Precision, owner-reviewed (the gate).** Run
   `python3 scripts/commitment_guard_review.py --limit 30` in your own
   Terminal. In shadow the draft is what was sent, so the script pairs each
   detected event with your first outbound message in the next 180 s. It
   shows the message on screen only and asks whether it really was a
   commitment of that kind. It stores only the aggregate tally (`--out`).
   PASS needs `all.precision >= 0.8` over at least 30 labelled cases, and no
   kind below 0.6 with at least 5 cases.
3. **Calendar works.** At least 80% of the `kind=plan when=1` lines read
   `calendar=free` or `busy`. If not, fix permission first; otherwise LIVE
   holds every plan.
4. **Latency.** Over the `prefilter=1` lines, p95 of
   `detect_ms + calendar_ms` is at most 1500 ms. The prefilter-miss rate
   (`prefilter=0` share) is at least 70%, so most turns pay nothing.
5. **Prefilter recall (advisory).** Over the `audit=1` lines, fewer than 10%
   have `kind != none` at `conf >= 0.6`. More than that means the prefilter
   lists need widening before LIVE is trusted.

## Rollback

Unset the gate, or set `HU_COMMITMENT_GUARD=off`, in the launchd plist via
`scripts/install-human-daemon.sh`. Never hand-edit the plist or `cp` over the
running binary. Then run `scripts/verify-deploy.sh`. OFF runs nothing, and the
reply is exactly the previous path. The calendar helper is inert unless the
gate is on. Delete `~/.local/bin/hu-calendar-free-busy` to remove it; the
guard then reads `calendar=unknown`.
