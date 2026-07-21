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

Also observed (worth a follow-up, out of scope here): our outbound typing does
not appear to take effect — `setLocalUserIsTyping:1 returned, isCurrentlyTyping
after=0`, with `acctLoggedIn=0` in the same log line.

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
