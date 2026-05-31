# Agent-Core Boundary — No Concrete Provider/Channel Knowledge

`src/agent/` is the orchestration core. It must not know about concrete
providers or channels by name:

1. **No provider factory** — agent code receives a `hu_provider_t` vtable
   injected; it must not `#include "human/providers/factory.h"` and
   instantiate providers itself.
2. **No channel-name `memcmp`** — channel identity knowledge belongs in the
   Channels context behind `hu_channel_behavior_class_for_name()`
   (`include/human/channels/behavior_class.h`), not in branches like
   `memcmp(cn, "imessage", 8)` inside the turn loop.

## Why

These are the T3 leaks: the orchestration core coupled to specific
infrastructure. They make the agent loop know channel identities (violating
open/closed — adding a channel edits agent core) and defeat dependency
injection for providers.

## Ratchet, not absolute

Both checks are ratchets (baselines measured 2026-05-29: factory **4**,
channel-memcmp **11**). They fail only on growth, so the guard is green today
and safe in pre-commit. The phases that retire them lower the baseline:
- **Phase 1** (channel `behavior_class`) removes the turn-loop memcmp →
  set `MEMCMP_BASELINE=0`.
- **Phase 4** (provider injection / app-config facade) removes the factory
  includes → set `FACTORY_BASELINE=0`.

## Enforcement

`scripts/check-agent-core-boundary.sh`, wired into `.githooks/pre-commit`
(fires when a `src/agent/` file is staged).

## Related

- `docs/plans/2026-05-29-ddd-bounded-contexts/README.md` — Phase 1 (channel `behavior_class`, ✅ done) in the completed ledger
- `docs/plans/2026-05-29-ddd-bounded-contexts/phase-E4-repackaging.md` — provider injection retires `FACTORY_BASELINE`
- `~/.claude/rules/substring-classifier-pitfalls.md` — why the prefix `memcmp` is also a latent bug
