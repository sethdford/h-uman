---
title: RadixAttention Upstream Watch (Phase 7 unblock signal)
created: 2026-05-24
status: frozen-pending-upstream
parent: docs/plans/2026-05-24-gemma-throughput-program.md
blocker: mlx-lm continuous batching
---

# Phase 7 — RadixAttention upstream watch

Phase 7 in the throughput plan is "cross-user RadixAttention-style
prefix sharing." Per the four-stream research pass that authored the
plan, this delivers **4-6× throughput on prefix-heavy workloads** (SGLang's
production measurement) — exactly the shape of multi-user persona-shared
prefixes that human's per-channel agent loops produce.

The reason this phase is frozen rather than under active development:
**RadixAttention needs continuous batching to be useful, and mlx-lm's
continuous batching support hasn't shipped yet.** Multiple sources in
the May 2026 research pass (Apple-internal MLX commits, Awni Hannun's
threads, the SGLang vs. MLX comparison) flagged this as the constraint.

This doc captures what an unblock looks like AND a way to detect it
without ceremony.

## Trigger condition

mlx-lm ships continuous batching when ANY ONE of these is true:

1. **`mlx-lm` release notes mention "continuous batching" or "paged
   attention" as a SHIPPED feature** (not roadmap, not "planned").
   Watch: <https://github.com/ml-explore/mlx-lm/releases>
2. **mlx-lm's source contains a `paged_kv_cache.py` / `continuous_batching.py`
   module on `main` branch.** Watch:
   <https://github.com/ml-explore/mlx-lm/tree/main/mlx_lm>
3. **Awni Hannun (@awnihannun) tweets / blogs that CB is operational
   on M-series** — this is usually the leading signal because mlx-lm
   ships fast after his confirmation.

The automated check script (below) tests #1 and #2 via the GitHub API
without auth requirements.

## What changes when the trigger fires

The implementation has three layers:

### Layer 1 — Cross-request prefix cache (radix tree)

A SGLang-style radix tree keyed on token sequences. When request B's
tokens start with a prefix that's already in the cache from request A,
B reuses A's prefix KV pages.

Integration site: in our codebase, the natural home is a NEW module
`src/providers/kvcache_radix.c` that sits ALONGSIDE the existing
`llamacpp_kvcache.c` (Phase 2's LRU). The LRU stays as the
single-context cache; the radix tree is a process-level cache shared
across contexts/providers.

The radix-tree algorithm itself is straightforward (~300 LOC). The
hard part is plumbing the lookup into the chat path before tokenization
even happens — so the bench doesn't pay tokenization cost on a hit.

### Layer 2 — Paged KV cache from mlx-lm

When mlx-lm exposes `paged_kv_cache_init(tokens) → kv_handle`, we can
pre-populate handles on cache hits without re-running attention.

Integration site: `scripts/mlx-server.py` `_chat_completion_inline` and
`_stream_chat_completion`. The handle ID gets passed through the
existing prompt-cache reuse path.

### Layer 3 — Per-channel prefix discovery

Operators don't see RadixAttention as a feature — they see "the
telegram channel's persona block stops costing TTFT once two users
share it." We need a small bit of telemetry at the operator level
that surfaces hit-rate per-channel (similar to Phase 0.3's per-cache
hit/miss counters, but per-prefix-pattern).

Integration site: a new `hu_kvcache_radix_t` struct with the same
`hits` / `misses` atomic counter pattern as Phase 2's LRU, plus a
per-prefix-bucket `tokens_shared` counter.

## Estimated implementation effort once unblocked

| Layer | LOC | Time |
|---|---|---|
| Radix tree data structure + tests | ~300 | 1-2 days |
| Chat-path integration (pre-tokenization lookup) | ~150 | 1 day |
| mlx-server.py paged-KV bridge | ~100 | 1 day |
| Operator telemetry + doctor surfacing | ~150 | 0.5 day |
| End-to-end bench-day verification | — | 0.5 day |

**Total: ~4-5 days** once mlx-lm's continuous batching ships.

That's about the same as the Phase 1-4 train *combined* — Phase 7 is
the single largest implementation cost in the plan, and the cost is
justified by the 4-6× throughput claim.

## Automated check

Drop the following into your `/loop` rotation or a cron job. It hits
the mlx-lm GitHub API and reports the unblock signal as soon as either
trigger condition fires:

```bash
#!/usr/bin/env bash
# scripts/check-mlx-lm-cb-upstream.sh
#
# Phase 7 unblock detector. Exits 0 with a message when mlx-lm has
# shipped continuous batching; exits 1 with "still waiting" otherwise.
# Suitable for /loop 24h or a daily cron.

set -euo pipefail

REPO="ml-explore/mlx-lm"
API="https://api.github.com/repos/${REPO}"

# Check 1 — latest release notes for the trigger phrase.
latest_notes="$(curl -s "${API}/releases/latest" \
                    | python3 -c 'import json,sys; r=json.load(sys.stdin); print(r.get("body",""))' \
                    2>/dev/null)"

if echo "${latest_notes}" \
        | grep -iE 'continuous.{0,4}batching|paged.{0,4}attention' >/dev/null; then
    echo "✅ Phase 7 UNBLOCKED — mlx-lm release notes mention CB / paged attn:"
    echo "${latest_notes}" | grep -iE 'continuous.{0,4}batching|paged.{0,4}attention' | head -3
    echo
    echo "Next step: docs/guides/radix-attention-upstream-watch.md §implementation"
    exit 0
fi

# Check 2 — module presence on main.
tree_json="$(curl -s "${API}/contents/mlx_lm?ref=main")"
for path in paged_kv_cache.py continuous_batching.py paged_attention.py batch_scheduler.py; do
    if echo "${tree_json}" | grep -q "\"name\": \"${path}\""; then
        echo "✅ Phase 7 UNBLOCKED — mlx-lm/main contains ${path}"
        echo
        echo "Next step: docs/guides/radix-attention-upstream-watch.md §implementation"
        exit 0
    fi
done

# Still gated.
echo "Phase 7 still gated on mlx-lm continuous batching."
echo "Last checked: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "Latest mlx-lm release: $(curl -s ${API}/releases/latest | python3 -c 'import json,sys;r=json.load(sys.stdin);print(r.get("tag_name","?"))' 2>/dev/null)"
exit 1
```

Save as `scripts/check-mlx-lm-cb-upstream.sh`, mark executable, and
optionally wire to `/loop`:

```
/loop 24h scripts/check-mlx-lm-cb-upstream.sh
```

When it fires (exit 0), you'll get a notification with the unblock
evidence. At that point this doc's §implementation section becomes
the work plan.

## Why not just implement the radix tree now?

Considered and rejected. Without continuous batching:

- The radix tree CAN'T share pages across active sequences — each
  llama_context has its own KV memory, and you can't transplant KV
  from one context to another safely
- Best-case is "the SAME llama_context holds N prefix snapshots" —
  which is the Phase 2 multi-slot LRU we already shipped, just with
  a fancier data structure
- The cost of building 300 LOC + tests for a system that delivers
  Phase 2's behavior with extra complexity is unjustified

The 4-6× throughput claim depends on:
1. **Multiple concurrent requests** (continuous batching)
2. **A shared KV memory region** (paged attention)
3. **Lookup BEFORE tokenization** (so the tokenizer cost isn't paid
   on hits — radix tree on token IDs lets us match without re-tokenizing)

Item 1 and 2 don't exist in mlx-lm yet. Item 3 we can build today, but
it's the cheapest 30% of the work and useless without 1 and 2.

So the discipline is: wait for the dependency. The auto-checker fires
the moment we shouldn't be waiting anymore.

## Related

- [Phase 2 multi-slot LRU](../../src/providers/llamacpp_kvcache.c) — what we have today
- [Phase 2b would-skip counter](../../include/human/providers/llamacpp_kvcache.h) — the per-context measurement that would extend to per-prefix in Phase 7
- [Plan §Phase 7](../plans/2026-05-24-gemma-throughput-program.md) — original framing
- [Bench-day runbook](bench-day-runbook.md) — the workflow Phase 7 would extend
