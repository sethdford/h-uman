# `imsg watch --bb-events` — Observed Event Schema

**Status:** empirically captured 2026-07-19 on this Mac Studio (Tahoe 26.5.1,
Apple Silicon), `imsg` 0.11.0, SIP **disabled**, bridge **live**.

Everything below is either (a) verbatim stdout captured from a running
`imsg watch --bb-events`, or (b) explicitly labelled **UNCONFIRMED**. Nothing
here is inferred-and-presented-as-fact — per
`~/.claude/rules/audit-verify-before-allege.md`.

---

## 0. Bridge preconditions (verified, not assumed)

```
$ imsg status
System Integrity Protection (SIP):  disabled
Advanced features (typing, read receipts):
  Available - IMCore bridge connected
  bridge version: v2 (v2 inbox active)

$ lsof -p $(pgrep -x Messages) | grep -c imsg-bridge
2
```

The `lsof` check matters: `DYLD_INSERT_LIBRARIES` can be set while macOS
silently refuses to load the dylib. The env var is **not** proof; an open
handle on `imsg-bridge-helper.dylib` inside the Messages process is.

---

## 1. Transport: a JSONL file inbox, not a socket

The injected dylib **appends** event records to a file in the Messages sandbox
container, and `imsg watch --bb-events` tails it:

| Path | Role |
|---|---|
| `~/Library/Containers/com.apple.MobileSMS/Data/.imsg-events.jsonl` | event inbox (dylib writes, CLI tails) |
| `…/.imsg-events.jsonl.1` | rotation target |
| `…/.imsg-bridge-ready` | liveness sentinel |
| `…/.imsg-bridge.log` | dylib debug log (very useful for triage) |
| `…/.imsg-rpc/{in,out}` | v2 command RPC (separate from events) |

**Consequence for h-uman:** the stream can go silent for two very different
reasons — no events happened, or the bridge died. They are indistinguishable
from stdout alone. The `.imsg-bridge-ready` sentinel is the liveness check.

## 2. The CLI re-wraps — this is the contract that matters

`imsg` does **not** pass inbox lines through. Proven by injecting a known line
and reading stdout:

```
inbox  → {"event":"started-typing","data":{"chatGuid":"…","handle":"…","timestamp":1784510000.5}}
stdout → {"event":"started-typing","data":{…verbatim…},"kind":"bridge-event"}
```

Envelope rules, established by differential injection:

| Inbox line | stdout `event` | stdout `data` |
|---|---|---|
| has top-level `event` (string) | that string | — |
| no top-level `event` | `"unknown"` | — |
| has top-level `data` (object) | — | that object, verbatim |
| no top-level `data` | — | `{}` |
| any | `kind` is always the literal `"bridge-event"` | |

**h-uman parses stdout, so `kind == "bridge-event"` is the discriminator.**

## 3. Verbatim captures

### 3.1 Bridge event lines (captured from live `imsg watch --bb-events`)

```json
{"event":"started-typing","data":{"handle":"+1801XXXXXXX","timestamp":1784510000.5,"chatGuid":"iMessage;-;+1801XXXXXXX"},"kind":"bridge-event"}
{"kind":"bridge-event","event":"stopped-typing","data":{"timestamp":1784510006.0999999,"chatGuid":"iMessage;-;+1801XXXXXXX","handle":"+1801XXXXXXX"}}
{"kind":"bridge-event","event":"aliases-removed","data":{"aliasType":"phone","aliases":["+1484XXXXXXX"]}}
```

⚠️ **KEY ORDER IS NOT STABLE.** Line 1 leads with `event`; lines 2–3 lead with
`kind`. Swift's dictionary encoding does not preserve insertion order. Any
parser that relies on field position, or on `strstr` offsets assuming order,
**will** break intermittently. Parse by key.

⚠️ **`timestamp` is a float** (`1784510006.0999999`), Unix epoch seconds —
not an integer, not an ISO-8601 string like the message rows use.

### 3.2 A normal chat.db message row on the SAME stream (for contrast)

```json
{"guid":"229CFD8D-…","chat_id":16,"text":"bb-events probe B","is_from_me":true,
 "id":66417,"created_at":"2026-07-20T01:22:33.327Z","sender":"+1801XXXXXXX",
 "chat_guid":"any;-;+1801XXXXXXX","reactions":[],"attachments":[], …}
```

No `kind` key. Uses `created_at` (ISO-8601), `id` (rowid), `text`.

### 3.3 A reaction row (`--reactions`, chat.db-derived — **not** a bridge event)

```json
{"is_reaction":true,"is_reaction_add":true,"reaction_type":"love","reaction_emoji":"❤️",
 "reacted_to_guid":"91136D96-…","guid":"C14BFF8A-…","chat_id":16,"id":66416,
 "text":"Loved “bb-events probe A”","is_from_me":true, …}
```

Note the `chat_guid` mismatch across families: message rows carry
`any;-;+1801XXXXXXX` while bridge events carry `iMessage;-;+1801XXXXXXX`.
**Do not join these two on `chat_guid` without normalising the service prefix.**

## 4. What the bridge actually emits — the honest inventory

The dylib registers observers for exactly **two** IMCore notifications
(`strings` on `imsg-bridge-helper.dylib`, cross-checked against `--help`):

- `IMChatItemsDidChangeNotification` → typing (it looks for `TypingChatItem`)
- `__kIMAccountAliasesRemovedNotification` → `aliases-removed`

So the complete event vocabulary is **three** values:
`started-typing`, `stopped-typing`, `aliases-removed`.

### This contradicts the optimistic framing. Corrected capability table:

| Wanted capability | Delivered by `--bb-events`? | Reality |
|---|---|---|
| (a) typing started/stopped as an INPUT | **YES** — the only real win | the one genuinely new signal |
| (b) read receipts on OUR sent messages | **NO** | no observer registered for it |
| (c) tapbacks the instant they land | **NO** (already covered) | comes from `--reactions`, chat.db-derived |
| (d) edit/unsend notifications | **NO** | no observer registered for it |

`imsg status` on this box also reports `editMessage: ✗` and
`sendMessageReason: ✗` — the edit *send* selectors aren't resolving either.

**Net:** the entire value of `--bb-events` to h-uman is **inbound typing**.
That is still a top-tier humanness signal (don't double-text into someone's
half-typed sentence), but it is one signal, not four.

## 5. UNCONFIRMED — read this before building on it

**No inbound typing event was captured from real traffic.** During live probing
the inbox stayed at 0 bytes. The `started-typing` / `stopped-typing` lines in
§3.1 came from **injecting** records into the inbox and reading what the CLI
emitted — which proves the *transport and envelope* exactly, but does **not**
prove the `data` field names the dylib itself writes for a real typing event.

Why it couldn't be captured: only a **remote** party typing produces the event.
Our own `imsg typing` is outbound — the bridge log shows `handleTyping:` firing
and writing nothing to the inbox. Confirming this needs a second person or
device typing to this Mac.

Field names `chatGuid` / `handle` / `timestamp` are taken from the dylib's own
string table (adjacent to the event-kind literals), so they are *likely* right —
but they are **inferred, not observed**. The parser is therefore written to be
tolerant: an event whose `data` lacks a recognised chat key still parses as a
typing event with an empty chat id, rather than being dropped.

Also observed: our outbound typing *appeared* not to take effect —
`setLocalUserIsTyping:1 returned, isCurrentlyTyping after=0`, with
`acctLoggedIn=0` in the same log line. **Both numbers are phantoms.** See §8.

**To close this gap:** have someone text this Mac, run
`imsg watch --bb-events --json`, and diff a real line against §3.1.

## 6. Reproduce

```bash
imsg status | head -20                                  # bridge must be "connected"
lsof -p $(pgrep -x Messages) | grep -c imsg-bridge      # must be > 0
imsg watch --bb-events --reactions --json | tee capture.jsonl
# then have a REMOTE party type at you
```

## 7. Correction to `README.md`

That doc records SIP as *enabled* and advanced features *unavailable* — true
when written, **stale now**. As of 2026-07-19 SIP is disabled and the bridge is
live, so the Tier-1 go/no-go gate in the plan is **passed**.

## 8. Outbound typing: the bridge's two phantom readbacks

**Verified 2026-07-21** (imsg 0.11.0, macOS 26.5.1, bridge live). The two
alarming numbers in `handleTyping` are **hardcoded zeros**, not measurements.
Outbound typing is *not* known to be broken — the evidence that said it was
is invalid.

### 8.1 The finding

`handleTyping` logs `isCurrentlyTyping after=%d` and `acctLoggedIn=%d`. Both
read selectors that **do not exist** on this OS:

| Bridge reads | Class | Exists? | Real API on macOS 26 |
|---|---|---|---|
| `isCurrentlyTyping` | `IMChat` | **NO** | `localUserIsTyping` (`B16@0:8`) |
| `loggedIn` / `isLoggedIn` | `IMAccount` | **NO** | `loginStatus`, `isConnected`, `isRegistered`, `isOperational` |
| `setLocalUserIsTyping:` | `IMChat` | YES (`v20@0:8B16`) | — the *write* is fine |
| `supportsSendingTypingIndicators` | `IMChat` | YES | — logs `supportsTyping=1` correctly |

Guarded by `respondsToSelector`, a missing selector yields `0`. So both fields
print `0` **unconditionally, forever, on success and failure alike.**

Confirmed by selector-reference dump — the bridge binds `isCurrentlyTyping`
but **never** binds the real getter `localUserIsTyping`:

```
$ otool -ov imsg-bridge-helper.dylib | grep -iE 'typing|loggedin'
    0x275c9 supportsSendingTypingIndicators
    0x2816e isCurrentlyTyping          <-- does not exist on IMChat
    0x2823e loggedIn                   <-- does not exist on IMAccount
    0x286a7 setLocalUserIsTyping:      <-- exists; the write executes
```

Reproduce the selector check:

```python
# python3 — introspect the live IMCore in the shared cache
import ctypes, ctypes.util
objc = ctypes.cdll.LoadLibrary(ctypes.util.find_library('objc'))
ctypes.cdll.LoadLibrary('/System/Library/PrivateFrameworks/IMCore.framework/IMCore')
objc.objc_getClass.restype = objc.sel_registerName.restype = ctypes.c_void_p
objc.objc_getClass.argtypes = objc.sel_registerName.argtypes = [ctypes.c_char_p]
objc.class_getInstanceMethod.restype = ctypes.c_void_p
objc.class_getInstanceMethod.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
cls = objc.objc_getClass(b'IMChat')
print(bool(objc.class_getInstanceMethod(ctypes.c_void_p(cls),
                                        objc.sel_registerName(b'isCurrentlyTyping'))))  # False
```

`check-typing-status` (bridge RPC) is affected by the same defect: it reads the
same dead selector, so it returns `"typing": false` in **every** state,
including immediately after a successful `start-typing`. Do not use it as an
oracle.

### 8.2 The addressing hypothesis is disproven

`--chat-guid 'iMessage;-;…'`, `--chat-guid 'any;-;…'`, `--chat-identifier`,
`--to`, and `--to --service imessage` were run back-to-back. **All five**
resolve to the identical `IMChat` — `guid=any;-;<handle> supportsTyping=1
alreadyTyping=0 acctService=iMessage acctActive=1`. The chat registry
normalises every input form to the same object.

Every one of the 2,473 chats in `chat.db` has guid prefix `any` — there is no
`iMessage;-;` guid on this machine. That prefix is a *chat-identifier* form,
not a guid form; on macOS 26 chats are service-agnostic and IMCore picks
iMessage-vs-SMS per message at send time.

```sql
sqlite3 ~/Library/Messages/chat.db \
  "SELECT substr(guid,1,instr(guid,';')-1) pfx, COUNT(*) FROM chat GROUP BY pfx;"
-- any|2473
```

h-uman's Tier-1 `imcore_start_typing` (`src/channels/imessage.c`) already tries
`iMessage;-;`, `SMS;-;`, **and** `any;-;`, so it is correct as written.

### 8.3 What is NOT established

That outbound typing *does* reach the remote. The evidence for failure is
invalid, which is not the same as evidence of success. Both remaining checks
need a second device:

- `imsg watch --bb-events` fired at the **self-chat** produced no echo — but
  IDS does not loop typing back to the originating device, so this is
  inconclusive, not negative.
- `log stream --predicate 'process == "imagent"'` is redacted for those
  processes on this box (1 line captured); it cannot confirm the IDS send.

**The only ground truth is a human looking at a second device** while
`imsg typing --to <handle> --duration 6s` runs. Until someone does that, treat
outbound typing as *unverified*, not *broken* — and do **not** gate the lever
off on the strength of the phantom readbacks.

### 8.4 Rule

Do not treat `isCurrentlyTyping`, `acctLoggedIn`, or `check-typing-status` as
signals. They are constants. This is
`.claude/rules/ground-truth-over-proxy-signals.md` one level deeper than usual:
the CLI's `{"status":"completed"}` is a proxy, and the *diagnostic log line you
reach for to check the CLI* is **also** a proxy — a hardcoded zero. Verify a
readback's selector exists before believing what it reports.

### 8.5 Why h-uman's Tier-1 IMCore path can never fire (verified 2026-07-21)

`imessage_start_typing` (`src/channels/imessage.c`) is a 3-tier chain:
Tier 1 in-process IMCore → Tier 2 AX compose field → Tier 3 `imsg typing` CLI.

**Tier 1 is structurally dead in the daemon**, and this is not fixable by
adding prefixes. `IMChatRegistry.sharedInstance` *does* resolve out-of-process,
but `existingChatWithChatIdentifier:` returns nil for **every** prefix, because
a non-Messages process has no loaded chats:

```
IMChatRegistry class: 0x1f13a5428
sharedInstance      : 0x105944cd0        <-- registry available
  iMessage;-;+1801XXXXXXX  -> not found
  SMS;-;+1801XXXXXXX       -> not found
  any;-;+1801XXXXXXX       -> not found  <-- all three nil
```

Consequence: `imcore_start_typing` returns `false` at the `if (!chat)` guard —
*before* reaching `setLocalUserIsTyping:` — and the chain correctly falls
through. The daemon log confirms it: it says `typing started via imsg CLI`,
never `via IMCore`.

This also means the "Tier 1 returns `true` unconditionally after the setter"
concern is **moot** — that line is unreachable out-of-process. Do not add a
`localUserIsTyping` verification readback there; it would be dead code guarding
a dead path.

**The CLI tier is the real production path**, and it works precisely because
`imsg` drives the setter *inside* Messages, where the registry is populated.

Historical `all typing tiers failed` lines in `~/.human/logs/service-loop-error.log`
are `hu_log_info_once` — one per daemon lifetime, accumulated over months, and
mostly pre-date the bridge going live (2026-07-20). The most recent entries in
that log are `typing started via imsg CLI`. Do not read the raw count as a
current failure rate.

### 8.6 Note on `--duration` at the call site

Tier 3 passes a hardcoded `--duration 5s`. This is **not** a dead
"realistic duration" lever: the vtable signature
`start_typing(ctx, recipient, recipient_len)` carries no message length, and
the daemon drives `start_typing` → generate → `stop_typing` (`src/daemon.c`),
so the visible duration is bounded by real generation time. The `5s` is a
safety ceiling for the case where `stop_typing` never lands.
`hu_imessage_typing_duration` remains used by the legacy AppleScript
`imessage_simulate_typing` path.

### 8.7 imsg 0.11.0 → 0.13.1 upgrade: does NOT fix the phantoms (measured 2026-07-21)

Upgraded the CLI via homebrew (`steipete/tap/imsg`, MIT-licensed) and re-ran the
selector census against the new dylib. **None of the three bugs are fixed at
0.13.1** (latest release, 2026-07-17):

| Selector | 0.13.1 binds it? | Exists on macOS 26 IM classes? |
|---|---|---|
| `isCurrentlyTyping` | **still bound** | NO (real: `localUserIsTyping`) |
| `loggedIn` | **still bound** | NO (real: `loginStatus`/`isConnected`) |
| `editMessage…backwardCompatabilityText:` (no `newPartTranslation:`) | **still bound** | NO (real inserts `newPartTranslation:`) |
| `latestTypingIndicatorSendTimeInterval` (the fix) | not bound | YES |

0.13.1 binds **331** selectors (up from 230); the +101 are almost entirely
rich-link / URL-preview / dominant-color additions. The absent-from-IM count
*grew* 127→198. Live repro on 0.13.1 still logs
`setLocalUserIsTyping:1 returned, isCurrentlyTyping after=0  acctLoggedIn=0`.

**Method note:** the census is a static `otool -ov` of the on-disk dylib vs the
live IMCore runtime — it does NOT require re-injecting into Messages. So this
verdict holds without a Messages relaunch. (The running system was left with the
0.13.1 CLI driving the still-injected 0.11.0 dylib; `status`, `typing`, and
`send-rich` were all verified working across that version skew, so the upgrade
did not disrupt the daemon.)

### 8.8 Upstream independently confirms the stale-selector pattern — and it's fixable

The imsg CHANGELOG shows the maintainer hitting and fixing this EXACT bug class,
which is strong external corroboration that §8.1's diagnosis is real:

- **v0.12.0:** *"restore `chat-delete` on macOS 26 by falling back from
  `deleteChat:` to `IMChatRegistry._chat_remove:`, while failing closed when
  neither selector is available."* — `deleteChat:` was in our absent-selector
  census. Same bug, same diagnosis, already shipped.
- **v0.13.2 (unreleased):** *"restore group participant add/remove on macOS 26 by
  … probing both current and legacy IMChat selectors."* — same idiom again.

But **no released version touches typing, edit, or account-status selectors.**
Those three remain live bugs at HEAD.

### 8.9 Fork vs contribute — recommendation

- **License: MIT** (verified from the repo LICENSE). Forking is legally clean.
- **Upstream is healthy:** v0.13.1 released 4 days before this writing; commits
  2–3×/week; maintainer responds same-day; community selector fixes merged with
  attribution (`#146`, `#185`).
- **Our changes are exactly the shape upstream already accepts** (probe
  current+legacy selector, fail closed). Three one-line-ish fixes:
  `localUserIsTyping`/`latestTypingIndicatorSendTimeInterval` for typing,
  `newPartTranslation:` for edit, `loginStatus`/`isConnected` for account.

**Recommendation: CONTRIBUTE, don't fork.** Forking a fast-moving MIT repo to
carry three selector fixes means permanently rebasing against 2 releases/week —
the expensive path to the cheap outcome. Fork ONLY for h-uman-specific verbs
upstream has no reason to carry (focus-status reads, native send-later).

**Regardless of fork/contribute: add a boot-time selector-conformance check.**
Every one of the 16/16 drifted selectors failed *silently* (nil or hardcoded 0).
An assertion that every bound selector resolves at startup — logging loudly when
one doesn't — turns a two-day phantom hunt into one log line. This is the durable
win and it's h-uman-side, independent of upstream. See
`.claude/rules/feature-gate-requires-measurement.md` applied to the API surface.

### 8.10 The big opportunity: imsg binds ~1% of IMCore

imsg binds 133 IM selectors; IMChat/IMAccount/IMMessage/IMChatRegistry alone
expose 11,520. Verified-present, unbound, high-value-for-better-than-human:

- **Typing verification:** `latestTypingIndicatorSendTimeInterval` (`d16@0:8`) —
  the oracle that answers §8.3 with no second device.
- **Focus/DND awareness:** `canShareFocusStatusWithCompletion:`,
  `isMessagesAuthorizedToAccessFocusStatus`, `_supportsFocusMode` — don't
  double-text into a silenced thread; calibrate expected latency. Humans see the
  banner and ignore it; h-uman could act on it every time.
- **Native Send Later:** `_supportsSendLater`,
  `editScheduledMessageItem:scheduleType:deliveryTime:`,
  `cancelScheduledMessageWithGUID:destinations:cancelType:` — OS-delivered at a
  human hour, and *revisable/cancelable before it lands* (no human can revise a
  message they already sent).
- **Real delivery/read ground truth:** `IMMessage.timeRead`/`timeDelivered`/
  `timePlayed`/`isRead`/`isDelivered` — imsg binds 0 of 28 read-receipt
  selectors. This is the measurement substrate for real human-latency
  distributions, not chat.db inference.
- **Authoritative per-chat capability probes:** `_supportsEditMessage`,
  `_supportsSendLater`, `_supportsExpressiveText`, `_supportsDeliveryReceipts`
  (all `B16@0:8`) — replace `imessage_caps.c`'s brittle `imsg status`
  string-parsing, which currently inherits imsg's false negatives.

CAVEAT: existence ≠ behavior. `_supportsSendLater` resolving does not prove
send-later works when driven from the injected dylib. Prototype in a dylib and
measure before committing to any of these.
