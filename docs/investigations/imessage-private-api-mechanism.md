---
title: iMessage Private-API Mechanism — Reverse-Engineered Blueprint for a Native h-uman Port
date: 2026-05-31
feature: Native iMessage action surface via IMCore (reply, tapback, edit, unsend, delete, typing)
status: Blueprint — implementation phased below; Phase 2 gated on SIP-off + on-device verification
source: jesec/imessage-rs (MIT), studied at /tmp/imessage-rs-study 2026-05-31
---

# iMessage Private-API Mechanism — Blueprint

**Goal:** replicate `jesec/imessage-rs`'s approach *natively in h-uman* (no Rust
dependency, no vendored library) so the daemon can perform real native reply /
tapback / edit / unsend / delete / typing through Apple's private **IMCore**
framework. This doc is the reverse-engineered mechanism + the port plan.

> Source studied: `jesec/imessage-rs` (MIT license), `crates/imessage-private-api/`.
> All selectors below are quoted from its Swift `IMHelper` sources.

## The architecture (3 components)

```
┌─────────────────────────┐         localhost TCP            ┌──────────────────────────┐
│  h-uman daemon (C)       │   port 45670+uid-501, JSON\r\n   │  Messages.app            │
│  • TCP server (binds)    │ <══════════════════════════════ │  + injected helper dylib │
│  • injection orchestrator│   {action,data,transactionId}    │  (Swift/ObjC, arm64e)    │
│  • per-action JSON cmds  │ ════════════════════════════════>│  • dlsym IMCore symbols  │
│  • imessage channel hook │   {transactionId,...} / {event}  │  • performSelector calls │
└─────────────────────────┘                                  └──────────────────────────┘
```

1. **Helper dylib** — loaded into Messages.app via `DYLD_INSERT_LIBRARIES`.
2. **Injection orchestration** — daemon writes the dylib, `killall Messages`,
   relaunches Messages with the env var, hides it, retries.
3. **TCP server** — daemon binds a per-user localhost port; dylib connects back
   and exchanges newline-delimited JSON.

**Hard prerequisite: SIP disabled** (`csrutil disable` from Recovery). Without
it, `DYLD_INSERT_LIBRARIES` into an Apple-signed, library-validated binary is
refused. This is the user's manual, machine-wide step — it cannot be automated.

## Component 1 — the helper dylib

### Entry point (`DylibEntry.swift`)
Registered via the linker `-init` flag, exported `@_cdecl("_dylib_init")`. On
load it checks `Bundle.main.bundleIdentifier` for `com.apple.MobileSMS` /
`com.apple.Messages`, then `DispatchQueue.main.async { IMHelper.bootstrap() }`.

### Bootstrap (`IMHelper.swift`)
1. `resolvePrivateSymbols()` — `dlsym(RTLD_DEFAULT, ...)` for IDS + IMCore
   functions (notably `IMCreateThreadIdentifierForMessagePartChatItem`).
2. **Eagerly init IMCore singletons** so the first action doesn't hang 30-120s:
   touch `IMAccountController.sharedInstance.activeIMessageAccount` (forces the
   imagent/IDS connection) and `IMChatRegistry.sharedInstance`.
3. Connect the TCP client; on connect send `{"event":"ping"}` then `{"event":"ready"}`.
4. `installSwizzles()` for typing-indicator *reads*.

### Symbol resolution (`PrivateSymbols.swift`)
`dlsym(RTLD_DEFAULT_PTR /* (void*)-2 */, "IMCreateThreadIdentifierForMessagePartChatItem")`
cast to a `@convention(c)` fn pointer. IDS service-name globals are read as
`UnsafeRawPointer` to the CFString pointer. **No linking against private
frameworks** — everything is resolved at runtime, which is what keeps the dylib
loadable across macOS versions.

### Runtime-helper idioms (`PrivateAPI.swift`) — the port must mirror these
- `getSharedInstance(cls)` → `NSClassFromString(cls).perform("sharedInstance")`.
- `safePerform` / `safePerformReturning` → `responds(to:)` guard then `perform`.
- For methods with non-object args/returns (ranges, ints, the init below), it
  casts the IMP to a `@convention(c)` typedef and calls directly.
- `runtimeAlloc` → `cls.perform("alloc")` WITHOUT init (IMEmojiTapback /
  IMStickerTapback / IMTapbackSender crash on `init`).

## Component 2 — injection orchestration (`injection.rs`)

```
dylib_path = ~/Library/Application Support/<app>/private-api/imessage-helper.dylib
write embedded dylib → dylib_path
app_bin = /System/Applications/Messages.app/Contents/MacOS/Messages   (fallback /Applications/…)
loop (≤5 failures):
    killall Messages ; sleep 1
    if server.is_connected(): return        # already injected
    spawn app_bin with env DYLD_INSERT_LIBRARIES=dylib_path
    after 5s: osascript "set visible of process \"Messages\" to false"
    wait(child)  # on clean exit, reset failure count and re-inject
```

h-uman already spawns subprocesses (the `imsg watch` pattern in
`src/channels/imessage.c`) — the orchestrator is a direct analog. **Must use
the atomic-install discipline** (`.claude/rules/never-cp-over-running-binary.md`)
when writing the dylib.

## Component 3 — the TCP protocol (`TCPClient.swift`)

- **Port:** `clamp(45670 + getuid() - 501, 45670, 65535)`. Daemon binds; dylib
  connects, **retries every 5s** on failure/close.
- **Framing:** send JSON + `\r\n`; parse lines on `\n` (strip optional `\r`).
- **Request (daemon → dylib):** `{"action": "...", "data": {...}, "transactionId": "..."}`.
- **Response (dylib → daemon):** `{"transactionId": "...", ...}` or
  `{"transactionId": "...", "error": "..."}`.
- **Events (dylib → daemon, no transaction):** `{"event":"ping|ready|started-typing|stopped-typing|...","guid":...}`.
- Quirk it handles: duplicated JSON `}\n{` — truncate at the first.

## The IMCore selectors (the irreplaceable part)

All via `getChat` → `IMChatRegistry.sharedInstance` `existingChatWithGUID:`
(**Tahoe fallback:** rewrite GUID prefix `iMessage;-;` / `SMS;-;` → `any;-;`),
and `getMessageItem` → `IMChatHistoryController.sharedInstance`
`loadMessageWithGUID:completionBlock:`. The part chat item comes from
`message._imMessageItem._newChatItems`, matched by part index via
`__kIMMessagePartAttributeName`.

| Action | Mechanism |
| --- | --- |
| **Send text** | `IMMessage` alloc → `initWithSender:time:text:messageSubject:fileTransferGUIDs:flags:error:guid:subject:balloonBundleID:payloadData:expressiveSendStyleID:` (flags `0x100005` text / `0x10000d` subject / `0x300005` audio) → `chat sendMessage:`. Read back `chat lastSentMessage .guid`. |
| **Threaded reply** | Build the message as above, then set `threadIdentifier`. Resolve it from the parent (`message.threadIdentifier`); if empty, `IMCreateThreadIdentifierForMessagePartChatItem(item)` (dlsym'd). The parent is found via the `selectedMessageGuid` in the command `data`. **(Correction: it uses BOTH — `selectedMessageGuid` to locate the parent chat item, `threadIdentifier` as the actual IMMessage property.)** |
| **Classic tapback** | `IMMessage initWithSender:…associatedMessageGUID:associatedMessageType:associatedMessageRange:messageSummaryInfo:`. Type IDs **2000–2005** (add love/like/dislike/laugh/emphasize/question), **3000–3005** (remove). assocGuid = `p:<part>/<guid>` (text/attachment) or `bp:<guid>` (no item text). summaryInfo `{amc:1, ams:<parent text>}`. |
| **Emoji tapback** | `IMEmojiTapback` + `IMTapbackSender` (runtimeAlloc, no init). |
| **Sticker tapback** | `IMStickerTapback` + `IMTapbackSender`. **(Note: this is a sticker *reaction*, not a standalone sticker message — sticker SEND remains unimplemented, consistent with our `imessage_sticker.c` finding.)** |
| **Edit** | Tahoe: `chat editMessageItem:atPartIndex:withNewPartText:newPartTranslation:backwardCompatabilityText:` (5-arg). Sequoia: `…editMessageItem:atPartIndex:withNewPartText:backwardCompatabilityText:` (4-arg). Try Tahoe selector first via `responds(to:)`. |
| **Unsend** | `chat retractMessagePart:` on the part chat item. |
| **Delete (local)** | `chat deleteChatItems:` (array of items). Local only — does not retract for the recipient. |
| **Typing (read)** | swizzle `IMChat._handleIncomingItem:` (Sequoia) and `CKConversationListStandardCell.setShowTypingIndicator:` (Tahoe) → emit `started/stopped-typing` events. |
| **Markdown formatting** | attributed-string attrs `__kIMText{Bold,Italic,Underline,Strikethrough}AttributeName` + mandatory `__kIMMessagePartAttributeName`. |

## Corrections to the earlier spike doc

`imessage-imcore-private-api-spike.md` (2026-05-31) is now superseded on two points,
verified against primary source:
1. **Tahoe is supported**, not "closing window" — imessage-rs ships explicit
   Sequoia/Tahoe selector variants (edit 4-arg vs 5-arg; `any;-;` GUID rewrite;
   Tahoe typing swizzle). The vector is alive on macOS 15 **and** 26.
2. **Threaded reply uses `threadIdentifier`** (via
   `IMCreateThreadIdentifierForMessagePartChatItem`), *with* `selectedMessageGuid`
   to locate the parent — not one instead of the other.

The spike's core **NO-GO-for-default-on still holds**: SIP-off is a machine-wide
security downgrade incompatible with privacy-by-architecture as a default. This
ships as an explicit power-user opt-in tier, gated OFF, per
`feature-gate-requires-measurement.md`.

## Port plan for h-uman (phased)

### Phase 1 — verifiable C foundation (no SIP, unit-testable now)
- `src/channels/imessage_private/protocol.c` — pure helpers: the port formula
  `clamp(45670+uid-501, …)`, JSON line framing (`\r\n` out, `\n` in, `}\n{`
  dedup), transaction-id map. Pin with unit tests (mirrors the Swift behavior).
- Config schema: `channels.imessage.private_api.{enabled,mode}` default **OFF**,
  parsed + one-shot disabled-log (`silent-config-gated-subsystems.md`).
- No dylib yet → nothing speculative ships enabled; the foundation is exercised
  by tests, not by a live caller (documented as Phase-1-of-N).

### Phase 2 — the Swift helper dylib (needs SIP; on-device verify)
- New build target: `apps/imessage-helper/` Swift package → `imessage-helper.dylib`
  (arm64e, `-init _dylib_init`), the IMHelper port (subset: send, reply, tapback,
  edit, unsend, typing). Compiled only on macOS, behind a CMake/Swift opt-in.
- Cross-language boundary = the TCP socket (`cross-language-via-http.md`): the C
  core never links Swift/IMCore.

### Phase 3 — daemon wiring + injection
- TCP server in the daemon; injection orchestrator (atomic dylib install +
  `killall`/relaunch/hide/retry). `imessage` channel vtable gains a
  private-API backend that the dispatcher prefers when `is_connected()` — else
  falls back to the **hardened Tier-1 path already shipped in PR #242**.

### Phase 4 — gate + measure
- OFF→SHADOW→LIVE. Flip to LIVE only on a blind-A/B showing native
  reply/tapback materially beats the Tier-1 quote
  (`feature-gate-requires-measurement.md`).

## What cannot be verified in CI / here
Injection, IMCore calls, and the dylib require **SIP disabled + a real
Messages.app session + a physical reboot** — none reproducible in CI or this
sandbox. Phase 1 is the only fully CI-verifiable slice. Phases 2-4 require
Seth's machine with SIP off and manual on-device verification before any LIVE flip.

## Cross-references
- `imessage-private-api-spike.md` — the go/no-go (superseded on the two points above).
- `imessage-capability-matrix.md` — canonical capability matrix.
- `src/daemon/daemon_message_router.c` — the Tier-1 fallback this augments.
- `~/.claude/rules/cross-language-via-http.md` — the socket boundary discipline.
- `~/.claude/rules/feature-gate-requires-measurement.md` — OFF→SHADOW→LIVE gate.
- `~/.claude/rules/never-cp-over-running-binary.md` — atomic dylib install.
