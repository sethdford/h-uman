---
title: Log privacy — no message text or handles in production logs
created: 2026-10-02
status: operator-facing
---

# Log privacy (`HU_LOG_CONTENT`)

Production runs with `HU_DEBUG=1`, and the daemon's stderr is appended to
`~/.human/logs/service-loop-error.log`. Until 2026-10-02 that log recorded
every inbound batch as `processing batch for <handle>: "<first 60 chars>"`
(491 lines in 13 days), plus reply previews, drafts, captions and a hex dump
of each reply's first 80 bytes (DEF-12).

## The rule

Log lines carry counts, lengths, enums, booleans and a **contact tag**, never
message text, drafts, names or handles.

Helpers (`include/human/core/log_redact.h`, `src/core/log_redact.c`), used with
`%s`:

| Macro | Default output | With `HU_LOG_CONTENT=1` |
|---|---|---|
| `HU_LOG_WHO(h, len)` / `HU_LOG_WHO_CSTR(h)` | `#7a50b4f77146`: keyed SipHash-2-4 of the handle, low 48 bits | the handle (first 24 bytes) |
| `HU_LOG_TEXT(t, len, max)` / `HU_LOG_TEXT_CSTR(t, max)` | `<23 chars>` | the first `max` bytes |

The key is 16 random bytes created once per install at
`<state dir>/log_tag.key` (mode 0600, `O_EXCL`), so a tag can be neither
reversed nor recomputed from a contact list without that file, and 48 bits
make collisions across a contact list negligible. Tags are stable across
restarts, so lines can still be counted per contact. If the state dir is
unavailable the daemon uses a per-process random key (unlinkable, not
stable). Test builds use a fixed in-memory key and never touch the state dir.

`scripts/imessage_reply_latency.py` reads the same key file (honouring
`HU_STATE_DIR`) to recognise the owner's own handles; without it, it still
matches raw handles in pre-2026-10-02 logs. The voice-first shadow line uses
the same tag.

The reply hex dump in `hu_service_run` is skipped entirely unless
`HU_LOG_CONTENT=1`.

## The switch

`HU_LOG_CONTENT=1` (exactly `1`) turns text and raw handles back on. It is
**off by default** and is a local debugging aid only; never set it in the
launchd plist. It is deliberately not `HU_DEBUG`, which production has on.

To check a log: `grep -cE 'processing batch for \+|"[^"]{20,}"' ~/.human/logs/service-loop-error.log`
should stop growing after the deploy. Rollback is a revert of the commit;
there is no runtime path back to logging text short of `HU_LOG_CONTENT=1`.

## When adding a log line

Pass handles through `HU_LOG_WHO` and any message, draft, reply, caption or
model-written direction through `HU_LOG_TEXT`. Model names, error strings,
gate modes and file paths are fine as they are.
