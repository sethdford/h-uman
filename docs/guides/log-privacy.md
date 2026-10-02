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
| `HU_LOG_WHO(h, len)` / `HU_LOG_WHO_CSTR(h)` | `#3fa2`: a 16-bit FNV tag, stable across restarts, the same fold as the voice-first shadow line | the handle (first 24 bytes) |
| `HU_LOG_TEXT(t, len, max)` / `HU_LOG_TEXT_CSTR(t, max)` | `<23 chars>` | the first `max` bytes |

The tag is pseudonymous, not anonymous: anyone holding the contact list can
hash it. It is enough to count lines per contact.

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
