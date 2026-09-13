# Edge-Context Isolation — Channels Depend on Contracts, Not Each Other

> **Auto-locked since 2026-07-27.** The "lower the baseline by hand" step below is
> now performed automatically by `ratchet_autolock` (`scripts/lib/ratchet.sh`) the
> next time this gate runs, and a decay target derived from this counter's own
> history is reported weekly. See `.claude/rules/ratchet-decay.md`.

A concrete channel implementation may include the channel vtable contract and
shared infra, but must not take on a **new** dependency on a *different*
channel's header.

## Why

`providers/`, `channels/`, and `tools/` form the codebase's one genuinely clean
hexagonal boundary (the anti-corruption layer for dozens of providers, channels,
and tools). The audit verified it holds today. This guard keeps it that way —
the failure mode it prevents is `discord.c` quietly reaching into `slack.h`,
which would couple two contexts that should only meet at the vtable.

## Ratchet, not absolute

The current cross-channel includes (baseline **6**, measured 2026-05-29) are
all legitimate: the **imessage** channel is one channel split across several
files (`imessage`, `imessage_reply`, `imessage_ingest`, `imessage_action`,
`imessage_balloon_decode`, `imessage_sticker`), and Meta platforms share
`meta_common.h`. Those are grandfathered. The guard fails only on **growth**.

Exempt from the count: a file including its own header, and the shared infra
headers `format`, `dispatch`, `contact_signature`, `channel_embed`,
`behavior_class`, `reaction_event`, `meta_common`.

## Enforcement

`scripts/check-edge-context-isolation.sh`, wired into `.githooks/pre-commit`
(fires when a `src/channels/` file is staged).

## Related

- `docs/plans/2026-05-29-ddd-bounded-contexts/README.md` — Model Access (edge) context
- `~/.claude/rules/cross-language-via-http.md` — sibling boundary discipline
