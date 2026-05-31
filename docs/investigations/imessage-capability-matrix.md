---
title: "iMessage Capability Matrix"
description: "Canonical reference for iMessage capabilities, implementation status, feasibility, and priority."
---

# iMessage Capability Matrix

## Overview

Canonical reference for every iMessage platform capability, our implementation status, user behavior data, technical feasibility, and priority. Cross-references existing investigation docs and third-party tools.

## Platform Capabilities

| Feature | User Behavior | Feasibility | Our Status | Priority | Notes |
| --- | --- | --- | --- | --- | --- |
| **Send text** | Core (100%) | Public API (imsg CLI or AppleScript) | Implemented | — | `imessage_send()` via `imsg send` (preferred) or osascript fallback; `use_imsg_cli` config key |
| **Receive/poll text** | Core (100%) | chat.db SQLite + imsg watch | Implemented | — | `hu_imessage_poll()` via chat.db; event-driven `imsg watch` subprocess when `use_imsg_cli` enabled |
| **Send attachment** | High (~40% of convos) | imsg CLI or AppleScript | Implemented | — | `imsg send --file` (preferred) with per-attachment AppleScript fallback |
| **Receive attachment path** | High | chat.db + attachment table | Implemented | — | `hu_imessage_get_attachment_path()` |
| **Classic tapback (send)** | Very high (70%+ of users) | JXA + AX automation | Implemented (fragile) | High | Requires AX permission, varies by macOS version. `HU_IMESSAGE_TAPBACK_ENABLED` opt-in. |
| **Classic tapback (read)** | Very high | chat.db types 2000-2005 | Implemented | — | `hu_imessage_build_tapback_context()` |
| **Custom emoji reaction (read)** | Growing (iOS 17+) | chat.db type 2006 | **Implemented** (this overhaul) | — | Added in this overhaul, aggregated in tapback context |
| **Custom emoji reaction (send)** | Growing | imsg CLI `--reaction` or AX sub-picker | **Implemented** | Medium | `hu_imessage_react_emoji_with_fallback` (vtable `react_emoji`), wired in dispatcher at `daemon.c:1061`/`:1085`. imsg CLI emoji `--reaction` preferred; AX emoji sub-picker fallback; nearest-classic-tapback last resort. |
| **Proactive image generation** | High | DALL-E 3 + iMessage attachment | **Implemented** | — | 2% trigger, LLM prompt → DALL-E → download → send as attachment |
| **Read receipts (read)** | ~77% of users enable | chat.db date_read column | Implemented | — | `hu_imessage_build_read_receipt_context()` |
| **Read receipts (send)** | — | No public API | Not possible | — | Apple controls this per-contact |
| **Typing indicator (send)** | High (humanizing) | System Events AX automation | **Implemented** (AX-based) | — | System Events AX automation: focus Messages.app input field + keystroke to trigger real '...' bubble. No private entitlements needed. Requires Accessibility permission. |
| **Typing indicator (read)** | — | No public API | Not possible | — | Ephemeral, not stored in chat.db |
| **Inline reply (read)** | Moderate | chat.db thread_originator_guid | Implemented | — | `hu_imessage_lookup_message_by_guid()` |
| **Inline reply (send)** | Moderate | AX automation (3-tier) | **Implemented** (fragile) | High | `hu_imessage_reply` (vtable `reply`), wired in dispatcher at `daemon.c:1022`. 3-tier: Cmd-R via CGEvent → AX Show Menu→"Reply" → flat-send fallback (one-shot WARN). Real AX workers compile only under `HU_IMESSAGE_TAPBACK_ENABLED`; requires Accessibility permission, else degrades to flat send. |
| **Edit message (send)** | 1 in 50 messages | Not feasible (see investigation) | Not implemented | — | See `imessage-edit-feasibility.md`. IMCore private API only. |
| **Unsend message** | 1 in 50 messages | Partially feasible (AX automation) | Not implemented | Low | See `imessage-unsend-feasibility.md`. 2-minute window, fragile. |
| **Abandoned typing** | Humanizing pattern | Not feasible (see investigation) | Not implemented | — | See `imessage-abandoned-typing-feasibility.md` |
| **Music preview send (30s)** | Common | iTunes/Spotify API + AppleScript attachment | **Implemented** | — | iTunes .m4a preview + optional album art + Spotify/Apple Music rich link (auto-detected from user preference). Taste learning tracks reactions with persistent storage (~/.human/music_taste.json). Spotify token cached with expiry. See `src/multimodal/music.c`. |
| **GIF send** | Common | Tenor API + AppleScript attachment | Implemented | — | `hu_imessage_fetch_gif()` + send as attachment |
| **GIF tapback tracking** | — | chat.db | Implemented | — | `hu_imessage_count_recent_gif_tapbacks()` (now includes type 2006) |
| **Message effects (read)** | Common | chat.db expressive_send_style_id | **Implemented** (this overhaul) | — | Detects all 13 bubble/screen effects (Slam, Loud, Gentle, Invisible Ink, Confetti, Echo, Fireworks, Happy Birthday, Heart, Lasers, Shooting Star, Sparkles, Spotlight) and shows `[Sent with <effect>]` prefix in content. |
| **Message effects (send)** | Common | No public API | Not possible | — | Neither AppleScript nor imsg CLI supports sending effects. imessage-rs and Advanced iMessage Kit do via private API (requires SIP disabled). |
| **Sticker/Memoji (read)** | 1B+ Memoji weekly, 40% created | chat.db balloon_bundle_id | **Implemented** (this overhaul) | — | Detects stickers, Memoji, and iMessage apps via `balloon_bundle_id` in poll output. Shows [Sticker], [Memoji], or [iMessage App] in content. |
| **Sticker/Memoji (send)** | — | No public API | Not possible | — | No AppleScript/JXA/imsg/BlueBubbles path (verified, see `imessage-sticker-memoji-feasibility.md`). Vtable `send_sticker` is explicitly **NULL**; `hu_imessage_send_sticker` is a tested probe that returns `HU_ERR_NOT_SUPPORTED`. Expressive images go via the regular `.send` attachment path (renders as an image bubble, not a native sticker). |
| **Group chat (send)** | High | AppleScript (via chat name) | Implemented | — | Works with group chat identifiers |
| **Group chat (poll)** | High | chat.db | Implemented | — | Polls from all chats |
| **Contact allow-list** | — | Internal | Implemented | — | `allow_from` filter in create() |
| **Health check** | — | chat.db accessibility | Implemented | — | Checks if chat.db is readable |
| **attributedBody parsing** | Required for macOS 15+ | Binary blob parsing | **Implemented** (this overhaul) | — | Now shared across channels, feeds, persona sampler |
| **User activity detection** | — | chat.db is_from_me | Implemented | — | `hu_imessage_user_responded_recently()` |
| **Unsent message detection** | — | chat.db | Implemented | — | `was_unsent` field in poll output |

## Third-Party Tool Comparison

| Tool | Tapback Send | Typing | Edit | Unsend | Inline Reply | Speed | API Type |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **Our implementation** | imsg react (preferred) or JXA+AX; arbitrary emoji via `react_emoji` | Yes (AX) | No | No | Yes (AX 3-tier, fragile) | <1s send (imsg) | imsg CLI + AppleScript/AX fallback |
| **imsg v0.5.0** | Yes (`react` cmd) | Broken (macOS 26) | No | No | No | <1s send | AppleScript + Swift |
| **imsg-bridge** | Via imsg | Broken | No | No | No | ~1.2s avg | REST wrapper around imsg |
| **BlueBubbles** | Yes | Yes | Yes (private API) | Yes (private API) | Yes | ~2s | Private IMCore |
| **iMessage-Kit** | No | No | No | No | No | — | AppleScript wrapper |

## User Behavior Data (2025-2026)

- Tapback usage: 70%+ of iMessage users use tapbacks regularly
- Unsend usage: ~1 in 50 messages (higher than previously estimated)
- Edit usage: ~1 in 50 messages
- Sticker/Memoji usage: 1B+ Memoji stickers sent weekly; 40% of users have created a Memoji
- Read receipts: ~77% of users have them enabled
- GIF usage: Common in casual conversations, tracked via Tenor API integration
- Sources: worldmetrics.org 2026 iMessage statistics, Apple WWDC presentations

## Existing Investigation Cross-References

| Investigation | File | Conclusion | Changed Since? |
| --- | --- | --- | --- |
| Edit messages | `imessage-edit-feasibility.md` (2026-03-08) | Not feasible via public APIs | Re-confirmed 2026-05-31 (`imessage-imcore-private-api-spike.md`): IMCore `editMessage:atPartIndex:…` works but needs SIP-off + dylib, broken on Tahoe. NO-GO. |
| Unsend messages | `imessage-unsend-feasibility.md` (2026-03-08) | Partially feasible (AX automation) | Re-confirmed 2026-05-31: IMCore `retractMessagePart:` is the reliable path but Tier-2-only (SIP-off). AX path unchanged. |
| IMCore private-API spike | `imessage-imcore-private-api-spike.md` (2026-05-31) | Feasible via IMCore dylib; NO-GO for default-on (SIP + closing Tahoe window) | New — re-evaluated edit/unsend/reply; hardened Tier-1 fallback quote instead |
| Abandoned typing | `imessage-abandoned-typing-feasibility.md` (2026-03-10) | Not feasible | Minor — imsg v0.5.0 had typing but broken on macOS 26 |
| Typing indicators (AX) | `imessage-abandoned-typing-feasibility.md` (updated 2026-04) | AX workaround now implemented | Yes — AX typing works; abandoned typing still not feasible |

## Decision Log

| Decision | Rationale | Date |
| --- | --- | --- |
| Use AppleScript for send | Public API, no private deps, works across macOS versions | Original |
| JXA+AX for tapback send | Only viable public path for reactions | Original |
| Opt-in tapback via CMake flag | Fragile, requires AX permission | Original |
| Add custom emoji reaction read (type 2006) | iOS 17+ feature, read-only via chat.db, zero risk | 2026-04 |
| Extract attributedBody parser to shared | Needed by feeds + persona sampler, pure byte parsing | 2026-04 |
| Evaluate imsg CLI for tapback | Potentially more reliable than JXA, active maintenance | 2026-04 (pending) |
| Do not implement edit/unsend | Private API only, high maintenance risk | 2026-03 |
| Implement AX-based typing indicators | System Events focus + keystroke triggers real "..." bubble; no private entitlements; same AX permission as tapback | 2026-04 |
| Add sticker/Memoji read-side detection | balloon_bundle_id check in poll; zero risk read-only | 2026-04 |
| Add message effects read-side detection | expressive_send_style_id check in poll; zero risk read-only | 2026-04 |
| imsg CLI as unified runtime control | `use_imsg_cli` config key is single control; removed `HU_IMESSAGE_SEND_IMSG` CMake flag | 2026-04 |
| imsg CLI tapback even without JXA enabled | Auto-detect imsg on $PATH; try before returning NOT_SUPPORTED | 2026-04 |
| imsg send --file for attachments | Per-attachment imsg CLI with AppleScript fallback; faster and better errors | 2026-04 |
| imsg watch for event-driven polling | Spawns `imsg watch --json --since-rowid` subprocess; pipe-based notification; SQL skipped when no data | 2026-04 |
| imsg chats for target validation | One-time startup check: `imsg chats --json`; warns if default_target not in active chats | 2026-04 |
| Poll interval auto-reduction | Bootstrap sets 1s when `use_imsg_cli` enabled (vs 3s default); watch skips SQL when idle | 2026-04 |
| Implement 30s music preview attachments | iTunes + Spotify dual-service: .m4a preview, album art, auto-detect Spotify/Apple preference from history, taste learning via tapback reactions. | 2026-04 |

## Architecture

- **Outbound text**: `imessage_send()` → `imsg send --to --text` (preferred, <1s) → AppleScript fallback (2-5s)
- **Outbound attachments**: `imsg send --to --file` (preferred, per-attachment) → AppleScript `POSIX file` fallback
- **Inbound (event-driven)**: `imsg watch --json --since-rowid` subprocess → pipe notification → `hu_imessage_poll()` → chat.db SQLite (only when data available)
- **Inbound (fallback)**: timer-based `hu_imessage_poll()` → chat.db SQLite → parse text + attributedBody + attachments + reactions
- **Outbound typing**: `imessage_start_typing()` → osascript System Events → focus Messages.app input field + keystroke (triggers real "..." typing bubble)
- **Sticker/effects detection**: `hu_imessage_poll()` reads `balloon_bundle_id` and `expressive_send_style_id` from chat.db
- **Tapback send**: `imessage_react()` → `imsg react --chat-id --reaction` (preferred) → JXA + System Events AX fallback (opt-in)
- **Target validation**: `imsg chats --json` at channel start → warn if `default_target` not found
- **Feed/persona**: `feeds/imessage.c` and `persona/sampler.c` → chat.db with attributedBody support
- **Music preview**: taste prompt → LLM picks song → detect user preference (Spotify/Apple Music) → iTunes search (for .m4a) + optional Spotify search (for share link) → download preview + album art → send text + audio + artwork attachments → record for taste learning
- **GIF**: Tenor API → download to temp file → send as attachment

## Last Updated

2026-04-06
