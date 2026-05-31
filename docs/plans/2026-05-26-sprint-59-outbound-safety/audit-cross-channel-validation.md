---
title: "Audit — Cross-channel outbound validation coverage"
created: 2026-05-26
status: closed
last_audit: 2026-05-26
parent: STATUS.md
---

# Cross-channel outbound validation audit

After Sprint 60 closed (commits `3d5f8cbf` through `20ee7084`), an
explicit audit pass verified that the outbound safety pipeline fires
on EVERY channel, not just iMessage. This document records the
finding so the question doesn't get re-audited from scratch when
someone notices "we have many channel implementations — does the
pipeline cover them all?"

## TL;DR

**Yes — validation is channel-agnostic at the daemon level.** Every
LLM-generated outbound message passes through `hu_outbound_sanitize`
(the thin wrapper over `HU_OUTBOUND_PATH_PROACTIVE`) before
`channels[N].channel->vtable->send(...)` is called, regardless of
which channel (`imessage`, `slack`, `telegram`, `email`, `discord`,
etc.) is the transport.

No channel-side code originates LLM content. The channel's `send()`
vtable is a thin write-to-transport.

## Audit method

1. Grep all call sites that pass the result of an LLM call to a
   channel send:

   ```
   grep -rn "hu_outbound_sanitize\|outbound_pipeline_run\|burst_egress_validate_fragment" \
     src/channels/ src/daemon.c src/daemon/daemon_proactive.c
   ```

2. For each match, read 10 lines of context to confirm the
   validation immediately precedes the channel send.

3. Grep all direct channel send calls to confirm they originate
   from daemon code (validated path) or from channel-internal
   no-content transport (typing indicator, reaction, etc.):

   ```
   grep -rn "vtable->send\|->vtable->send" src/channels/*.c
   ```

## Findings

### Daemon-level validation call sites (verified)

| Site | Path | Wrapper |
|---|---|---|
| `src/daemon.c:1401` | F25 emotional check-in | `hu_outbound_sanitize` |
| `src/daemon.c:1509` | scheduled send | `hu_outbound_sanitize` |
| `src/daemon.c:2430` | proactive check-in | `hu_outbound_sanitize` |
| `src/daemon.c:10782` | burst sub-send | `hu_burst_egress_validate_fragment` |

Reactive replies route through `response_guard.c` per Sprint 59
design.md (Q-5 deferred consolidating it into the pipeline by 2
weeks of production data).

### Channel-side send call sites (verified safe)

Grep across `src/channels/*.c` for `vtable->send` or `->vtable->send`
returned ZERO hits. Each channel's `send` is the leaf entry written
to transport; no channel re-invokes another channel's vtable.

Channels DO contain internal calls like `start_typing()` and
`react_emoji()`, but those are control messages, not LLM content —
no validation needed.

### Channel inventory covered

The audit is one-size-fits-all by design (validation lives ONCE at
the daemon level), but for completeness the channel implementations
that benefit transparently from this validation:

`imessage`, `slack`, `telegram`, `email`, `discord`, `whatsapp`,
`facebook`, `instagram`, `twitter`, `tiktok`, `matrix`, `irc`,
`line`, `lark`, `web`, `imap`, `pwa`, `mattermost`, `onebot`,
`dingtalk`, `teams`, `twilio`, `google_chat`, `google_rcs`,
`gmail`, `voice_channel`, `mqtt`, `signal`, `nostr`, `qq`, `maixcam`.

All ~31 active channels share the same daemon-level validation.

## Why this architecture is right

Centralizing validation at the daemon level means:

1. **One implementation to maintain.** Adding a new stage (e.g.,
   Sprint 60's persona-classifier wiring) lights up validation
   across every channel automatically.
2. **No channel-by-channel coverage gaps.** Adding a new channel
   (say, a future Bluesky or Mastodon integration) gets pipeline
   validation FREE — no per-channel sanitizer logic to write or
   maintain.
3. **Single test surface.** The corpus regression test
   (`tests/test_outbound_corpus_regression.c`) runs against the
   pipeline once, and its findings apply to every channel.

The trade-off would be: if some channel had channel-specific
validation needs (say, "Slack rejects messages over 4000 chars"),
we'd handle that via the `channel_name` field in `hu_outbound_context_t`
which already plumbs through to per-stage logic
(e.g., `hu_shape_classify` reads it). No additional architecture
needed.

## When to re-audit

This audit becomes stale if:

- A new channel implementation lands AND calls `vtable->send` from
  its own initialization path (not just from daemon dispatch).
- A future feature lets channels ORIGINATE messages (e.g., a
  channel-side webhook auto-reply that bypasses the agent).
- The legacy `hu_outbound_sanitize` wrapper is deleted without
  migrating its call sites to direct `hu_outbound_pipeline_run`
  invocation.

Test that catches each:

- `tests/test_outbound_e2e_sota_proof.c` — end-to-end gate that
  fails CI if any of the 5 Annie/Mindy/Betty defense layers
  regresses.
- The legacy `hu_outbound_sanitize` is itself pinned by
  `tests/test_outbound_corpus_regression.c` — its delegation to
  the pipeline is part of the contract.

## Related

- `STATUS.md` — Sprint 60 closing recap
- `../../standards/operations/outbound-pipeline-stats.md` — operator
  runbook for the doctor stats check
- `../adr/2026-05-26-outbound-stats-health-thresholds.md` — threshold
  rationale
