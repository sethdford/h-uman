---
title: Voice triggers v2 — more moments for a voice memo, gate and promotion
created: 2026-10-01
status: operator-facing
spec: docs/superpowers/specs/2026-09-28-voice-first-memos-design.md
---

# Voice triggers v2

Voice-first memos ([spec](../superpowers/specs/2026-09-28-voice-first-memos-design.md))
decide voice before the reply is written. The original triggers are audio
from them, a fixed list of heartfelt phrases, and a question of 8+ words.
Over 14 days of production (to 2026-10-01), `HU_VOICE_FIRST=live` made 136
decisions. For real contacts, 77 of 79 were `no_trigger`, so the would-voice
rate was about 1.3%. A memo almost never happened.

`HU_VOICE_TRIGGERS_V2=off|shadow|live` (default **off**) adds four more
moments. Code: `src/context/voice_triggers.c` (pure predicates, every
threshold named in `include/human/context/voice_triggers.h`). Wiring:
`hu_daemon_voice_first_prepare` in `src/daemon/daemon_voice_first.c`.

## The four triggers

They are considered only where the base decision is `no_trigger`. Audio,
logistics and the per-contact spacing rule (`HU_VOICE_MIN_GAP_SEC`, default
3 h) still come first. Checked in this order:

| Reason | Fires when | Constants |
|---|---|---|
| `story_inbound` | Their message is 140+ chars and has no link, **or** it is 80+ chars with 2+ sentences (3+ words each) and a past-tense or feeling word ("we went", "so nervous"). | `HU_VOICE_V2_STORY_*` |
| `long_gap_reconnect` | A close contact, and the owner's newest message in the turn's channel history is 3+ days old. | `HU_VOICE_V2_RECONNECT_GAP_SEC` |
| `late_evening_warmth` | 20:00–23:30 in the **owner's** local time (the clock of the machine running the daemon, not the contact's time zone), a close contact, 4+ words, and not logistics. | `HU_VOICE_V2_EVENING_*` |
| `memo_length_reply` | A close contact asked 2+ real questions (each 3+ words, not a short logistics question). | `HU_VOICE_V2_MEMO_MIN_QUESTIONS` |

"Close" means persona `dunbar_layer` is `intimate` or `close`, or
`relationship_type` is `family` or `romantic`.

`memo_length_reply` reads their message, not the planned reply budget. A first
version used the budget (`max_chars` ≥ 200) and saturated (review of #576). For
any contact whose measured reply p90 reaches the iMessage ceiling, the budget
is at 200 on every turn, so for Mindy (p90 240) it fired on "ok". A reply that
answers several questions is a paragraph, which people talk rather than type.
It stays separate from `story_inbound` because it reacts to questions, not
narration. Long multi-question messages are caught by `story_inbound` first.

**Weekly cap.** At most `HU_VOICE_V2_WEEKLY_CAP` (default **2**) v2 memos
per contact per rolling 7 days. `HU_VOICE_V2_WEEKLY_CAP=0` turns every v2
reason off without touching the gate. The cap is kept in `proactive_decisions`,
**one ledger per mode**:

- **LIVE** counts `trigger='voice_v2'`, `decision='send'`. It books a send only
  when the turn actually writes the memo (voice-first LIVE, contact on
  `HU_VOICE_DELIVERY_ONLY`). A v2 VOICE decision for an unlisted contact uses
  nothing up.
- **SHADOW** counts its own would-voices in `trigger='voice_v2_shadow'`. This
  simulates the cap during shadow and never consumes the LIVE cap. **Do not
  read `voice_v2_shadow` rows as memos sent**, and do not merge the two ledgers
  in a report.
- **Fails closed.** If the ledger cannot be read (no decision log, a read error,
  or a build without SQLite), the cap counts as reached: v2 never chooses voice
  and the line shows `week=unknown`.

## Gate semantics

`HU_VOICE_TRIGGERS_V2` is evaluated inside voice-first, so it does nothing
unless `HU_VOICE_FIRST` is `shadow` or `live`.

- **off**: no v2 work, no v2 log line and no v2 row. The decision, the context
  and `max_chars` are byte-identical to before (test
  `test_voice_triggers_v2_off_is_byte_identical`).
- **shadow**: one aggregate line per voice-first decision (an owner `#voice`
  self-test does not get one):
  `[HU_VOICE_TRIGGERS_V2 shadow] base=<base reason> reason=<v2 reason|base|none|weekly_cap> pre=<trigger before the cap|none|-> would_voice=0|1 contact=<16-bit tag> owner=0|1 close=0|1 week=<n|unknown|->`.
  - `pre` keeps the per-reason split readable after the cap fills.
    `reason=weekly_cap pre=story_inbound` means a story that the cap held back.
  - It never logs text, names or handles. The contact tag is a 16-bit FNV hash
    of the handle. It is **pseudonymous, not anonymous**: anyone with the
    contact list can hash each handle and match every tag. It keeps the handle
    out of the log and does nothing more. Treat the log as identifying.
  - Where v2 evaluated, it also writes one `proactive_decisions` row
    (`trigger='voice_v2_shadow'`, `sent=0`).
  - The turn is unchanged (test
    `test_voice_triggers_v2_shadow_does_not_change_the_decision`).
- **live**: a v2 reason sets the decision to VOICE. The turn then writes a memo
  under the existing voice-first rules: LIVE, on `HU_VOICE_DELIVERY_ONLY`, not a
  group.

## Bug fixed alongside (not gated)

In production, all 18 `spacing` decisions in the 14 days were on the owner's
own thread, and each came within 3 h of a `#voice` self-test. Spacing is per
contact. The self-test memo was logged as `trigger='voice_reply', sent=1`
with the same reason as a real memo, so it started the owner's spacing gap.
Self-tests are now logged with reason `self_test`
(`hu_daemon_voice_first_reply_reason`), and the spacing lookup skips that
reason (`hu_proactive_decisions_repo_last_sent_ts_except`). A memo a contact
actually received still starts the gap.

## Promotion: SHADOW → LIVE

1. Set `HU_VOICE_TRIGGERS_V2=shadow` (with `HU_VOICE_FIRST=live`, as today)
   for **7 days**.
2. Measure the would-voice rate for **close, non-owner** contacts from the
   service log:

   ```bash
   grep -h 'HU_VOICE_TRIGGERS_V2 shadow' ~/.human/logs/service-loop-error.log \
     | grep 'owner=0' | grep 'close=1' \
     | awk '{n++; if ($0 ~ /would_voice=1/) v++} END {if (n==0) {print "no lines: refuse"; exit 2} printf "n=%d would_voice=%d rate=%.3f\n", n, v, v/n}'
   ```

   - The target band is **5–12%** of replies to close contacts. The baseline is
     about 1.3% across all non-owner contacts.
   - Under 5%, the triggers are too narrow. Over 12%, memos risk becoming a
     habit rather than a moment.
   - Read the per-reason split from `pre`, which is not hidden by the cap:
     `grep -o 'pre=[a-z_]*' | sort | uniq -c`.
   - Also count `reason=weekly_cap` lines. If many close contacts hit the cap,
     the post-cap rate understates demand, and the cap is doing the work.
   - `n=0` is not a rate; do not promote on it.
3. **The owner listens before LIVE.** For the first 10 shadow lines with
   `would_voice=1` and a v2 reason, open that thread in Messages at the
   timestamp shown. The log holds no text, by design. For each one, answer
   "would a voice memo have been right here?". Proposed bar: **8 of 10 yes**.
   - To hear what a memo would sound like, send a `#voice` self-test from the
     owner's number with a similar message. It renders through the same voice.
     It no longer affects spacing.
4. Only then set `HU_VOICE_TRIGGERS_V2=live`. Start with Mindy only by narrowing
   `HU_VOICE_DELIVERY_ONLY`, as the voice-first spec's rollout does, and widen
   the list after a week.

**Rollback:** set `HU_VOICE_TRIGGERS_V2=off` (or unset it) in the launchd plist
via `scripts/install-human-daemon.sh`. Do not `cp` over the running binary or
hand-edit the plist. Then verify with `scripts/verify-deploy.sh`. Off is the
pre-v2 path exactly. `HU_VOICE_V2_WEEKLY_CAP=0` is a softer brake: the gate
stays on and every v2 reason is suppressed.

## Delivery config this gate does not change

- **`HU_VOICE_DELIVERY_ONLY`** is the allowlist. It is a comma-separated list
  of handles (spaces around commas ignored), each matched exactly and
  case-insensitively against the sender handle as the daemon sees it, e.g.
  `+15551234567,someone@icloud.com`.
  - For voice-first it is the family list. An unset or empty list means nobody.
  - For native recording (`src/channels/imessage.c`), a handle not on the list
    falls back to the attachment route.
  - To widen it, add handles in the same E.164 / email form.
- **`HU_VOICE_NO_ATTACHMENT`**: only the exact value `1` refuses a memo as a
  file (`hu_voice_record_may_attach`). With `0` or unset, if native recording
  is blocked (owner typing, `user_active` / `user_returned`) or fails before
  Send, the memo goes out as a **`.caf` file attachment** instead of falling
  back to text.
  - That is what the voice-first spec removed on purpose: the owner said
    "no no caf file".
  - A file attachment also looks less like a native voice message.
  - Flipping it to `0` is therefore not recommended without the owner's
    explicit OK.
