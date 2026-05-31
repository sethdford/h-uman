---
title: iMessage IMCore Private-API Spike — Reliable Native Reply / Tapback / Edit / Unsend
date: 2026-05-31
feature: Native iMessage action surface (reply, tapback, edit, unsend)
status: Feasible-but-Gated — go/no-go below
supersedes_review: imessage-edit-feasibility.md (2026-03-08), imessage-unsend-feasibility.md (2026-03-08)
---

# iMessage IMCore Private-API Spike

**Date:** 2026-05-31
**Status:** Feasible via private API; **NO-GO for default-on** today (SIP + Tahoe).
**Scope:** Re-evaluates the 2026-03 "edit/unsend not feasible via public API"
verdict against the 2025-2026 reality, and asks whether the private **IMCore**
dylib-injection path is worth adopting for *reliable* native replies/tapbacks.

## Why this spike exists

Our reply/tapback path is **real** — it drives Messages.app via Accessibility
(Cmd-R / AXShowMenu) and verifies threading by reading `thread_originator_guid`
back from `chat.db` (`src/channels/imessage.c:3643`, `:3918`). But it is
**fragile**: AX layout shifts (especially the macOS 26 SwiftUI Messages
rewrite), a missing Accessibility grant, or a non-last parent all degrade the
native thread to a flat send. Users perceive the degraded path as "fake."

The question this spike answers: **is there a more reliable mechanism, and what
does it cost?**

## The two-tier reality (2025-2026)

iMessage automation splits into two hard tiers. We are entirely on Tier 1.

| Tier | Mechanism | Privilege | What it can do |
| --- | --- | --- | --- |
| **Tier 1** (ours) | AppleScript / `imsg` CLI / `chat.db` / Accessibility UI-scripting | none (AX permission only) | send, read, monitor, **drive the visible Reply/Tapback UI** |
| **Tier 2** | Pre-compiled Swift/ObjC dylib injected into Messages.app via `DYLD_INSERT_LIBRARIES`, calling **IMCore** private frameworks | **SIP disabled** | reliable native reply, tapback, **edit**, **unsend**, typing, effects — direct, no UI scripting |

Tier 2 is the only path to *rock-solid* native threading and to edit/unsend at
all. It is what BlueBubbles and the Rust/ObjC reimplementations use.

## Exact IMCore selectors (verified across 3 independent implementations)

These were confirmed by adversarial verification across BlueBubbles' IMCore
docs, `jesec/imessage-rs`, and `iandwelker/smserver` (deep-research run
2026-05-31, 15/25 claims confirmed):

| Action | IMCore method / metadata | Notes |
| --- | --- | --- |
| **Tapback / reaction** | `-[IMChat sendMessageAcknowledgment:forChatItem:withMessageSummaryInfo:]` | type IDs **2000-2005** add (love/like/dislike/laugh/emphasize/question), **3000-3005** remove. Emoji/sticker tapbacks also supported. |
| **Threaded reply** | message constructed with **`selectedMessageGuid`** = parent GUID | NOT `threadIdentifier` — that claim was *refuted* (1-2) in verification. The reply path passes `selectedMessageGuid`. |
| **Edit** | `-[IMMessage editMessage:atPartIndex:withNewPartText:backwardCompatabilityText:]` | macOS 13+ (Ventura). **Part index is mandatory** for multi-part messages. |
| **Unsend** | `-[... retractMessagePart:]` on the correct `IMMessagePartChatItem` | resolve part index first. |
| **Sticker send** | **undocumented** — `is_sticker` column exists in `chat.db`, BlueBubbles v1.8.0 changelog mentions sticker fixes, but no public reverse-engineered send method. | Status unknown. Matches our existing `imessage_sticker.c` "Not Feasible (send-side)" finding. |
| **Forward** | **no documented mechanism anywhere** | No open-source toolkit implements it. Treat as not feasible. |

## Reference implementations (open source)

| Project | Language | Surface | State |
| --- | --- | --- | --- |
| **BlueBubbles** (`BlueBubblesApp/bluebubbles-server`) | Swift/ObjC helper + server | full Tier-2 (reply, tapback, edit, unsend, typing, effects) via IMCore | canonical; ~10k★, active. **NOTE:** verification *killed* every claim that BlueBubbles does these via AppleScript — it uses the IMCore private API, not UI scripting. |
| **jesec/imessage-rs** | Rust + build-time Swift dylib | IMCore exposed as REST (`POST /api/v1/message/{guid}/edit`, `/unsend`, `/react`) | cleanest modern selector reference. |
| **steipete/imsg** | Swift CLI | Tier-1 (`send`, `react`, `watch`, `chats`) — what **we already integrate** | `react` targets the most-recent inbound only (no per-GUID targeting). |

> ⚠️ Verification caveat (per `~/.claude/rules/audit-verify-before-allege.md`):
> the deep-research run also attributed findings to an `openclaw/imsg` repo that
> was **not independently confirmed to exist**. Treat BlueBubbles and
> imessage-rs as the trustworthy IMCore references; verify any `openclaw` URL
> before relying on it.

## The cost: SIP + the closing Tahoe window

Tier 2 requires:

1. **System Integrity Protection disabled** (`csrutil disable` from Recovery) —
   a major security downgrade for the whole machine, and a hard blocker for any
   user who won't do it.
2. **A signed helper dylib** injected via `DYLD_INSERT_LIBRARIES` into
   Messages.app — an ObjC/Swift component we don't currently build (our runtime
   is C11).
3. **Tolerance for breakage on macOS 26 / Tahoe.** Confirmed regression: Tahoe
   adds library-validation + XPC entitlement enforcement
   (`com.apple.imagent.desktop.auth`) that breaks dylib injection and rejects
   direct IMCore clients. Edit and typing are reported broken on Tahoe in the
   reference projects. **The production window for this vector is Sonoma /
   Sequoia, and it is closing.** macOS 27 (likely 2026-2027) may close it
   entirely.

This directly contradicts our project rules: SIP-off + a private-API dylib is
the opposite of *privacy-by-architecture / runs-anywhere*, and it violates
"no speculative abstractions" if built before a user demands it.

## Go / No-Go

**NO-GO for default-on Tier 2.** The cost (SIP disabled, an out-of-language
helper, a documented and closing Tahoe breakage) is not justified for a
persona daemon whose moat is privacy-by-architecture. The 2026-03 verdict on
edit/unsend ("private API only, high maintenance risk — do not adopt") **still
holds**, now with the added datapoint that the window is actively closing.

**GO for the Tier-1 hardening instead** (shipped alongside this doc): gate the
threaded-fallback inline quote on `hu_imessage_reply_should_quote_on_fallback`
so the bot-like `↩ "quote"` glyph only appears when a human would actually
reference the parent (parent not newest / stale / multiple pending questions).
This removes the most visible "fake" tell with zero SIP/dylib cost.

**CONDITIONAL re-open** if all three become true:
1. A user/segment explicitly accepts SIP-off (e.g. a power-user opt-in tier).
2. We have a measured blind-A/B showing native threading materially improves
   indistinguishability over the hardened Tier-1 quote (per
   `~/.claude/rules/feature-gate-requires-measurement.md`).
3. Apple has NOT closed the injection vector on the then-current macOS.

If re-opened, build it the cross-language way (`~/.claude/rules/cross-language-via-http.md`):
a separate signed Swift helper exposing IMCore over localhost HTTP/RPC (the
imessage-rs shape), gated OFF→SHADOW→LIVE behind config, never linked into the
C core.

## Decision-log entries (for the capability matrix)

| Decision | Rationale | Date |
| --- | --- | --- |
| Re-confirm: do NOT adopt IMCore private-API edit/unsend | SIP-off + dylib + closing Tahoe window; conflicts with privacy/runs-anywhere moats | 2026-05-31 |
| Harden Tier-1 fallback quote instead (gate on should_quote_on_fallback) | Removes the bot-like over-quoting tell at zero SIP cost | 2026-05-31 |
| Sticker send: status UNKNOWN, not "impossible" | `is_sticker` metadata + BlueBubbles changelog hint a private path exists but none is reverse-engineered | 2026-05-31 |
| Forward: not feasible | No documented mechanism in any toolkit or IMCore doc | 2026-05-31 |

## Cross-references

- `docs/investigations/imessage-capability-matrix.md` — canonical matrix; this spike updates the edit/unsend rows.
- `docs/investigations/imessage-edit-feasibility.md` (2026-03-08) — verdict re-confirmed here.
- `docs/investigations/imessage-unsend-feasibility.md` (2026-03-08) — verdict re-confirmed here.
- `src/daemon/daemon_message_router.c` — the THREADED branch hardened alongside this doc.
- `src/channels/imessage_action.c` — `hu_imessage_reply_should_quote_on_fallback` predicate.
