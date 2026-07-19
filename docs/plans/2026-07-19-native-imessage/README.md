# Native iMessage — Perfect, Blue, and Natural

**Goal:** h-uman sends iMessages the way Seth actually does — real threaded replies,
real tapbacks, effects, typing indicators, read receipts, edit/unsend — with
**zero green bubbles** and nothing that reads like a bot poking a UI menu. Better
than human where the OS allows (perfect cadence, never a missed read receipt).

**Status:** plan. Grounded in deep research (97-agent, adversarially verified,
2026-07-19) + empirical probing of THIS machine (Tahoe 26.5.1, Apple Silicon).

---

## The one hard truth (verified on this box, not guessed)

`imsg status` on this machine, right now:

```
Basic features (send, receive, history):   Available
System Integrity Protection (SIP):         enabled
Advanced features (typing, read receipts): Not available
```

Everything divides at exactly one line: **SIP**.

- **Blue + natural basic texting** works today, SIP-on, no system changes.
- **Full native fidelity** (tapbacks, threaded replies, typing, read receipts,
  effects, edit/unsend) runs through Apple's **private IMCore framework**, which
  macOS gates behind **library validation + private entitlements**. On Tahoe the
  only verified way past that gate is **SIP disabled**. This is a firm fact from
  `imsg status` and confirmed (MEDIUM/HIGH) across BlueBubbles, Barcelona, and
  mautrix-imessage — not a hedge.

I will **not** flip SIP for you — it weakens the whole machine's security posture
and it is genuinely your call. The plan is built so that **Tier 0 makes the
daemon "always blue" with SIP on**, and **Tier 1 unlocks full fidelity the moment
you choose to disable SIP**. You decide the gate; everything else is engineering.

### What the research killed (so we don't build on sand)

The adversarial verifier **refuted 3-0** the specific reverse-engineered selector
docs floating around GitHub (exact tapback ID ranges, `editMessage:atPartIndex:`
signatures, capability codes). Lesson baked into this plan: **never hardcode
selectors from stale docs.** We drive the bridge that already binds to the *live*
IMCore.framework on the target machine (`imsg`), or class-dump the framework
present at build time — never a blog post.

---

## The asset we already have

The `imsg` on this box is **[steipete/imsg](https://github.com/steipete/imsg)**
(v0.11.0, universal Mach-O). It is not a shell wrapper — it links
`/System/Library/PrivateFrameworks/IMCore.framework` and ships
`imsg-bridge-helper.dylib`. Its own binary strings prove the capability surface:

| String in the binary | Native capability it implements |
|---|---|
| `associatedMessageType` | Tapbacks / reactions (the real API, not a menu click) |
| `databaseReplyToGUID` | Threaded / inline replies that actually nest |
| `com.apple.messages.effect.CK` | Message effects (slam, loud, gentle, invisible ink, confetti, lasers) |
| "Send a message via the IMCore bridge (effects, replies, subjects)" | Send path w/ effects + subject |
| `IMChatRegistry`, `ReactionType`, `IMCoreBridge` | The bridge classes, bound to the live framework |
| typing / read-receipt strings | Typing indicators + read receipts |

**h-uman already shells to `imsg` for basic `send`/`react`/`watch`**
(`src/channels/imessage.c`). So Tier 1 is not "reverse-engineer IMCore in C" —
it's "enable the bridge `imsg` already has, and route h-uman's advanced verbs
through it instead of through the JXA/System-Events UI hack at `imessage.c:1470`."

---

## Current state (what "not how I do it" actually is)

Today h-uman's advanced iMessage behavior is **UI puppetry**:

- **Tapbacks:** `imessage.c:1470` drives `System Events` → `AXShowMenu` → clicks
  the Tapback menu with synthetic keystrokes. Fragile, visible, wrong.
- **Replies:** `⌘R`-into-last-message hack, gated behind "parent is the last
  message" because synthetic input **cannot set `thread_originator_guid`** — so
  replies don't truly nest (proven via self-chat, macOS 26).
- **Effects / edit / unsend / send-side typing / read receipts:** **not built.**
- **Blue guarantee:** none — send can silently fall back to AppleScript with no
  iMessage-vs-SMS assertion.

The bridge path replaces all of that with the real APIs.

---

## Architecture: a capability-probed, blue-guaranteed channel

```
                    ┌─────────────────────────────────────────┐
  h-uman daemon ──▶ │  hu_imessage_send / react / reply / …    │
  (channel vtable)  │                                          │
                    │  1. CAPABILITY PROBE (startup + cached)  │
                    │     parse `imsg status` → {basic, bridge}│
                    │  2. BLUE GUARD (always)                  │
                    │     assert handle is iMessage-reachable  │
                    │     else HOLD, never send green          │
                    │  3. ROUTE by capability:                 │
                    │     bridge? → imsg bridge verbs (Tier 1) │
                    │     else    → basic send + graceful      │
                    │              degrade (Tier 0)            │
                    └─────────────────────────────────────────┘
```

Every advanced verb is **gated on the probe**. SIP-on → Tier 0 behavior (still
blue, still natural for basic texting). SIP-off + bridge launched → Tier 1 lights
up automatically. No code path ever emits a green bubble or a UI-puppet tapback.

---

## Tier 0 — ships now, SIP untouched: **guarantee BLUE + natural basic**

Buildable today, no security change. Closes the "green bubble" and "UI hack" gaps.

**T0.1 — Blue-only send guard.** Before any send, verify the target resolves to
an **iMessage** service (not SMS). Query via IMCore-read or `imsg`'s service
check; if the handle is not iMessage-reachable, **HOLD and log** rather than fall
back to green. *Acceptance:* a non-iMessage handle is never sent to; test with a
known-SMS-only number → daemon holds, emits `blue_guard: held (not imessage)`.

**T0.2 — Retire the UI-puppet tapback.** Delete the `System Events`/`AXShowMenu`
JXA path (`imessage.c:1358–1560`). Until the bridge is up, a reaction that can't
be sent natively **degrades to a short in-voice text** ("haha" / "❤️") rather than
a menu-click — honest and human, never a fake tapback. *Acceptance:* grep shows
zero `AXShowMenu` in the send path; reaction with no bridge → text fallback, logged.

**T0.3 — Inbound fidelity (read side, already possible).** Parse
`thread_originator_guid`, `associated_message_type`, effects, and typing/read
signals **from chat.db** so the daemon *understands* threading/reactions it
receives even when it can't yet send them. Feeds the arena + persona. *Acceptance:*
inbound reply/tapback/effect surfaces in the turn context; unit test on a chat.db
fixture.

**T0.4 — Capability probe + doctor.** Add `hu_imessage_probe_caps()` that parses
`imsg status` once at startup, caches `{basic, bridge, sip}`, and surfaces it in
`human-daemon doctor imessage`. Everything Tier-1 gates on this. *Acceptance:*
doctor prints the live capability matrix; probe is the single source of truth.

---

## Tier 1 — the unlock, requires **your** SIP decision: full native fidelity

One-time setup (yours to run — I will not disable SIP for you):

```
1. Reboot → Recovery (hold power on Apple Silicon) → Terminal → `csrutil disable` → reboot
2. Full Disk Access for the daemon binary (already granted for iMessage read)
3. cd <imsg source> && make build-dylib          # builds imsg-bridge-helper.dylib
4. imsg launch                                    # injects bridge into Messages/imagent
5. imsg status  → "Advanced features: Available"  # PROVE it before wiring
```

**Tahoe caveat (verified honest):** even with SIP off, `imsg status` warns library
validation *may* still block the bridge on 26.x. Step 5 is the go/no-go — we wire
h-uman to Tier 1 **only after `imsg status` reports the bridge live on this box.**
If Tahoe blocks it even with SIP off, fallback is the BlueBubbles-helper injection
technique (MEDIUM-confidence, same SIP-off requirement) as an alternate bridge.

Once the probe reports `bridge: available`, h-uman routes these through it:

| Capability | Bridge mechanism (via `imsg`, bound to live IMCore) | h-uman wiring |
|---|---|---|
| **Threaded replies** | `databaseReplyToGUID` on the outgoing IMMessage | `hu_imessage_reply(parent_guid, text)` → `imsg` bridge send w/ reply-to |
| **Real tapbacks** | `associatedMessageType` (add + **remove**) | replace T0.2 fallback; react() → bridge |
| **Effects** | `com.apple.messages.effect.CK.*` | optional per-message effect arg, persona-gated (rare, like a human) |
| **Typing indicator** | IMCore typing set/clear | send "typing" for a compose-time proportional to reply length, then send — reads as real |
| **Read receipts** | IMCore mark-read | mark inbound read on the same cadence Seth would open the app |
| **Edit / unsend** | Ventura+ edit/retract (macOS 13+, verified) | `hu_imessage_edit(guid,newtext)` / `hu_imessage_unsend(guid)` — for guard catches: unsend a bad send instead of a correction text |
| **Subjects / mentions / audio** | bridge send w/ subject; group mention; audio attach | lower priority; wire after the core four land |

**Better-than-human levers (unlocked here):** typing-indicator duration matched to
actual compose time; reaction latency that mimics human reaction time (not instant);
read receipts on a natural rhythm; **unsend** to retract a response-guard catch
before it's read instead of the current "ignore my last text" correction. These are
things that make it read as *more* present than a human, not less.

---

## Build sequence

1. **T0.4 probe** (foundation — everything gates on it) → **T0.1 blue guard**
   → **T0.2 retire UI hack** → **T0.3 inbound fidelity**. Ships value now, SIP-on.
2. **Decision point:** you run the Tier-1 SIP setup + `imsg launch`, paste
   `imsg status`. If bridge live → proceed. If Tahoe blocks → BlueBubbles-helper
   alt or stop at Tier 0 (still blue + natural).
3. **T1 wiring** in dependency order: reply (biggest "how I do it" win) → tapback
   → typing/read-receipt → edit/unsend → effects/subjects/mentions/audio.
4. Each verb: gated on probe, unit-tested with a bridge stub, live-proven by
   sending to your self-chat and reading it back from chat.db, then fed through
   the **conversation arena** so the judge scores whether the new fidelity reads
   as more human.

## Risks / honest caveats

- **SIP off is a real security downgrade.** Your machine, your call. Tier 0 gives
  a genuinely good "always blue, always natural-basic" daemon without it.
- **Bridge is private API** → can break on any macOS point update. The probe +
  graceful degrade means a broken bridge falls back to Tier 0, never to green or
  UI-puppetry. `imsg` (steipete, maintained) tracks OS changes upstream.
- **Don't vendor stale selectors.** Always bind to the live framework via `imsg`
  or a build-time class-dump. (Research refuted the blog-doc selectors 3-0.)
- **Tahoe may block even SIP-off.** Unverified either way (research 0-3, i.e.
  unproven); Step 5 `imsg status` is the empirical gate before we wire anything.

## Sources

Deep-research report (verified findings): steipete/imsg, BlueBubbles
private-api docs + bluebubbles-helper, beeper/barcelona, mautrix-imessage,
ReagentX/imessage-exporter, Apple HT213207 (edit/unsend ≥ macOS 13). Local
ground truth: `imsg status`, `imsg` binary strings, `IMCore.framework` present,
`sw_vers` = 26.5.1.
