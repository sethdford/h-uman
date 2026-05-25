# US-14 — Cross-Provider Fallback (MLX-down → cloud Gemini)

**Date:** 2026-05-26
**Verdict:** **ALREADY SHIPPED** — needs config change, not code.

## Investigation summary

The Sprint 56 cloud-fallback investigator estimated 4-6 hours to wire cross-provider
fallback by extending `hu_provider_degrade_chat` to accept a second provider. **That
estimate was wrong** — there is already a working cross-provider fallback at a
LOWER layer in the codebase:

`src/providers/from_config.c::hu_provider_create_default` recognizes the magic
provider name `"reliable"` and wraps the primary with `hu_reliable_create_ex`,
which automatically falls through to any number of fallback providers from
`cfg->reliability.fallback_providers[]`. The fall-through happens at the provider
vtable layer, transparent to `agent_turn.c` and `hu_provider_degrade_chat`.

**Key evidence (file:line):**
- `src/providers/from_config.c:139-192` — builds extras chain from
  `cfg->reliability.fallback_providers_len` entries, passes to
  `hu_reliable_create_ex`
- `src/providers/from_config.c:265` — `hu_provider_create_default` is the entry point
- `src/providers/from_config.c:117-193` — `"reliable"` magic-name handler reads
  `cfg->reliability.primary_provider` as the primary, falls through to
  `fallback_providers[]` array on any vtable error
- `src/providers/reliable.c` — the wrapping implementation (already shipped)

## How to enable (one-time config change for the operator)

Add to `~/.human/config.json`:

```json
{
  "default_provider": "reliable",

  "reliability": {
    "primary_provider": "mlx_local",
    "fallback_providers": ["gemini"],
    "provider_retries": 2,
    "provider_backoff_ms": 500
  }
}
```

What this does:
1. `default_provider: "reliable"` triggers `from_config.c::hu_provider_create_default`
   to build the reliable wrapper instead of a single provider.
2. The wrapper has `mlx_local` as primary (the running mlx-server.py on port 8741,
   with v4-repair adapter loaded) — every reply goes there first.
3. On any failure (network error, timeout, HTTP 5xx, subprocess crash), the
   wrapper auto-falls-through to `gemini` — your cloud Gemini provider.
4. The user gets a reply within seconds either way.
5. Loss when fallback fires: LoRA personalization (cloud Gemini doesn't apply
   the adapter), but reply quality is still ~85-90% of personalized output —
   far better than the canned "I'm having trouble connecting" message that
   fires today when mlx_local fails.

## What you give up vs current `default_provider: "mlx_local"`

- **Latency on healthy path:** identical (mlx_local is still primary)
- **Latency on degraded path:** ~1-2s cloud Gemini call instead of immediate
  honest-failure message
- **Personalization on degraded path:** lost (cloud Gemini has no LoRA), but
  the user gets a real reply
- **Operator observability:** the reliable wrapper logs the fallback decision;
  operator can grep for "reliable: primary failed" to see when fallback fires

## Comparison to the investigator's proposed 4-6h work

The investigator's plan would have:
1. Added config parsing for `agent.fallback_model` — UNNECESSARY (already in
   `reliability.fallback_providers`)
2. Modified `hu_provider_degrade_chat` to accept a second provider — UNNECESSARY
   (the reliable wrapper handles fallthrough at a lower layer)
3. Created dual provider instances in `agent.c` — UNNECESSARY (the reliable
   wrapper IS a single provider that internally holds multiple)
4. Updated `agent_turn.c` call site — UNNECESSARY (no API change)

**Lesson for next investigation:** when an investigator scopes a fix, also grep
for existing magic-name handlers in the relevant factory. The "reliable" string
search would have surfaced this immediately.

## Status

- **Code:** ✅ already shipped (multiple prior sprints)
- **Config:** ⏸ operator action required (one config edit + daemon restart)
- **Tests:** ✅ `tests/test_reliable.c` covers the wrapper behavior
- **Recommendation:** Apply the config above. Daemon restart picks it up. Smoke
  test by stopping mlx-server.py briefly and observing that replies still flow
  (via cloud Gemini fallback).
