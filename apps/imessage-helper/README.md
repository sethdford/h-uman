# imessage-helper — h-uman's native IMCore private-API dylib

A Swift helper dylib injected into **Messages.app** via `DYLD_INSERT_LIBRARIES`
so the h-uman daemon can drive Apple's private **IMCore** framework: native
threaded reply, tapback, edit, unsend, delete, and typing — the actions no
public API exposes.

This is a clean-room native port of the approach used by `jesec/imessage-rs`
(MIT), studied and documented in
[`docs/investigations/imessage-private-api-mechanism.md`](../../docs/investigations/imessage-private-api-mechanism.md).
**No Rust, no vendored library** — the daemon (C) plays the role imessage-rs's
Rust core plays: it binds the TCP server, injects this dylib, and sends JSON
commands.

## ⚠️ Hard prerequisite: SIP disabled

`DYLD_INSERT_LIBRARIES` into Apple's signed, library-validated Messages.app is
**refused unless System Integrity Protection is disabled**. There is no
workaround — that is the entire purpose of SIP. This is a machine-wide security
downgrade and a deliberate power-user opt-in.

```
1. Shut down the Mac.
2. Boot into Recovery (hold Power on Apple Silicon).
3. Utilities → Terminal.
4. csrutil disable
5. Restart.
```

Without this, the daemon's injection step will spawn Messages.app but the dylib
will never load, and the h-uman dispatcher falls back to the hardened Tier-1
reply path (shipped in the fallback-quote gate). Nothing breaks; the private-API
backend simply stays unavailable.

## Build

```bash
# Compile-check / dev build (current arch):
bash apps/imessage-helper/build.sh

# Production artifact MUST be arm64e to match Messages.app and be code-signed:
bash apps/imessage-helper/build.sh --arm64e --sign "Developer ID Application: …"
```

The build emits `libIMHelper.dylib` with an `-init __dylib_init` constructor
that fires when the dylib loads into a target process.

## How it runs (orchestrated by the daemon — Phase 3)

1. Daemon binds localhost port `45670 + uid - 501` (see
   `src/channels/imessage_private/protocol.c`).
2. Daemon writes this dylib to
   `~/Library/Application Support/human/private-api/imessage-helper.dylib`
   (atomic install — never `cp` over a loaded dylib).
3. Daemon `killall Messages`, then relaunches
   `/System/Applications/Messages.app/Contents/MacOS/Messages` with
   `DYLD_INSERT_LIBRARIES=<dylib>`.
4. `_dylib_init` fires → `IMHelper.bootstrap()` → resolves IMCore symbols,
   eagerly inits IMCore singletons, connects back to the daemon's TCP server.
5. Daemon sends `{"action":"send-message"|"edit-message"|…, "data":{…},
   "transactionId":"…"}`; the dylib calls IMCore and replies
   `{"transactionId":"…", …}`.

## Scope of this port

Implemented (outbound action surface — the indistinguishability goal):
`send-message`, threaded reply (`selectedMessageGuid` + `threadIdentifier`),
classic tapback (types 2000-2005 / 3000-3005), `edit-message` (Tahoe 5-arg +
Sequoia 4-arg), `unsend-message` (`retractMessagePart:`), `delete-message`
(local `deleteChatItems:`), `start-typing` / `stop-typing`
(`setLocalUserIsTyping:`).

Deferred follow-ups (documented in the blueprint, not yet ported):
emoji/sticker tapback (`IMEmojiTapback`/`IMStickerTapback`/`IMTapbackSender`),
attachments/multipart, inbound typing-read swizzles, FaceTime/FindMy.

## Verification status

The Swift here is **compile-checked only**. Runtime behavior (injection +
IMCore calls) cannot be verified in CI or without SIP off — it requires a real
Messages.app session on a SIP-disabled Mac. See the on-device runbook in the
blueprint doc.
