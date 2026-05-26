# M3 Dispatch Unification — Design

## TL;DR

Turn `hu_init_proposer_tick_with_provider` into the single proactive
composer. Pass it the rich context daemon_proactive currently builds.
Daemon_proactive becomes the scheduler that loops contacts and feeds
each one through init_proposer.

```
BEFORE:                           AFTER:
daemon_proactive loop:            daemon_proactive loop:
  for each contact:                 for each contact:
    build rich prompt                 build rich-context bundle
    hu_agent_turn(prompt) ─┐          init_proposer.tick_with_provider(
    response_guard.check    │           cfg, ar_cfg, budget, recency,
    validator_chain         │           agent, provider, bundle, ...)
    vtable.send()  ─────────┘         on FIRED → vtable.send(draft)
                                      on else → log gate reason
init_proposer.tick (orphan):
  separate path, never feeds
  the daemon-side composer
```

## Architecture

### New shape of `hu_init_proposer_tick_with_provider`

Current signature ([init_proposer.h:234](../../include/human/agent/init_proposer.h)):

```c
hu_error_t hu_init_proposer_tick_with_provider(
    const struct hu_initiative_config *cfg,
    const struct hu_autoresponder_config *ar_cfg,
    int32_t tz_offset_seconds,
    struct hu_proactive_budget *budget,
    const struct hu_agent *agent,
    struct hu_provider *provider,
    hu_allocator_t *alloc,
    int64_t last_inbound_unix,
    int64_t now_unix,
    int64_t *last_tick_unix_inout,
    uint64_t *tick_id_inout,
    hu_init_proposer_result_t *out_result,
    hu_init_decision_t *out_decision);
```

Add **one new parameter**: a `const hu_proactive_compose_inputs_t *`
that carries the rich context (memory callbacks, weather, calendar,
feeds, channel name, recipient handle). This avoids breaking existing
callers — they pass NULL and get the current behavior; new callers
populate the struct.

```c
typedef struct hu_proactive_compose_inputs {
    /* Identity of the proactive target. */
    const char *contact_id;
    size_t contact_id_len;
    const char *channel_name;
    size_t channel_name_len;

    /* Pre-built context fragments. Lifetimes are tied to the caller's
     * stack frame for this tick; init_proposer copies what it needs
     * into the prompt. */
    const char *memory_context;   size_t memory_context_len;
    const char *weather_context;  size_t weather_context_len;
    const char *calendar_context; size_t calendar_context_len;
    const char *feeds_context;    size_t feeds_context_len;

    /* Output-safety: the caller's pre-applied outbound-safety filter
     * (see hu_daemon_callback_content_is_safe). init_proposer asserts
     * this is non-NULL when memory_context is non-empty. */
    bool (*content_is_safe)(const char *, size_t);
} hu_proactive_compose_inputs_t;
```

### Init_proposer's send wrap

Today, on FIRED, `hu_init_proposer_tick_with_provider` returns the
draft via `*out_decision`. The caller is expected to send it.

In the unified design, init_proposer **does NOT send directly** — it
returns the draft + result. The caller (daemon_proactive) sends via
the channel vtable. This preserves the test seam (init_proposer can
be unit-tested without a channel mock) AND lets the daemon apply its
existing throttle/recency/send-cap machinery uniformly.

The caller's wire looks like:

```c
hu_init_proposer_result_t result;
hu_init_decision_t decision;
hu_proactive_compose_inputs_t inputs = {
    .contact_id = cp->contact_id,
    .contact_id_len = strlen(cp->contact_id),
    .channel_name = channel_name,
    .channel_name_len = strlen(channel_name),
    .memory_context = memctx, .memory_context_len = memctx_len,
    .weather_context = wctx,  .weather_context_len = wctx_len,
    .calendar_context = cctx, .calendar_context_len = cctx_len,
    .feeds_context = fctx,    .feeds_context_len = fctx_len,
    .content_is_safe = hu_daemon_callback_content_is_safe,
};

hu_error_t err = hu_init_proposer_tick_with_provider(
    &cfg->initiative, ar_cfg, tz_off, &gov_budget, agent, provider,
    alloc, last_inbound, now, &last_tick, &tick_id, &result, &decision);

if (result != HU_INIT_RESULT_FIRED) {
    log_gate_reason(result, cp);
    continue;
}

/* Validator chain runs INSIDE init_proposer's send wrap.
 * On REJECT: retry. On retry-fail: skip. On OK: caller sends. */
channels[c].channel->vtable->send(
    channels[c].channel->ctx, target, target_len,
    decision.draft, decision.draft_len, NULL, 0);
hu_proactive_throttle_record_send(throttle, cp->contact_id, "proactive", now_ms);
hu_governor_record_sent(&gov_budget, now_ms);
```

### Validator chain placement

The validator chain (G1–G9 + persona_voice + identity-anchor retry)
runs INSIDE init_proposer's send wrap, NOT in the caller. This is the
single-source-of-truth for response shaping; the caller cannot forget
to apply it.

Specifically: after `hu_init_proposer_evaluate_decision` returns
FIRED, init_proposer:
1. Builds a `hu_guard_context_t` from the inputs (channel, persona).
2. Runs `hu_response_guard_check_ex` on `decision.draft`.
3. On REJECT, runs `hu_response_guard_retry_slim_with_identity` and
   captures the DPO negative pair via
   `hu_response_guard_log_dpo_negative`.
4. On retry-success, replaces `decision.draft` with the retry text.
5. On retry-fail, downgrades the result to `HU_INIT_RESULT_LLM_ERROR`
   so the caller skips the send.

The retry path is the same machinery agent_turn uses; init_proposer
just calls into it. Zero behavior duplication.

### Backwards compatibility

`hu_init_proposer_tick_with_provider` keeps its current signature. The
new compose-inputs parameter is added as an **extension** via a
second function `hu_init_proposer_tick_with_provider_ex` that wraps
the original and accepts the additional inputs. The original becomes
a thin wrapper that passes `inputs=NULL`.

This pattern matches the existing `hu_response_guard_check` /
`hu_response_guard_check_ex` shape — same h-uman convention.

### Behind a feature flag for the rollout

Add `cfg->proactive.use_unified_dispatch` (default false). For the
first week:
- false → daemon_proactive uses the existing
  prompt-build + agent_turn + validator-chain path.
- true → daemon_proactive uses
  `hu_init_proposer_tick_with_provider_ex`.

A/B for one week. If FIRED rate is comparable to the legacy path's
non-skip rate (within 5%), flip default true. After two more weeks of
stable production, delete the legacy path.

## Test seams

Three new test files, each scoped tight:

### `tests/test_init_proposer_compose.c`
- Pure-predicate tests for the new compose-inputs prompt builder.
- Truth table over (memory present, weather present, calendar present,
  feeds present, persona overlay present, channel name).
- Asserts the prompt contains exact substrings from each populated
  context source, and skips slots cleanly when a source is empty.

### `tests/test_proactive_dispatch_unified.c`
- Daemon-integration test with a mock provider that returns a fixed
  decision JSON and a mock channel that captures the send call.
- Walks the full path: daemon_proactive scheduler → init_proposer
  tick → provider call → decision parse → validator chain → channel
  send.
- Asserts (a) the channel send received the LLM-decided draft, NOT
  the rich prompt context.
- Asserts (b) when the LLM returns confidence=0.5 (below threshold),
  the channel send is NEVER called.

### `tests/test_proactive_dispatch_validator_chain.c`
- Injects a G9-tripping draft via the mock provider's decision.
- Asserts init_proposer's send wrap calls
  `hu_response_guard_check_ex`, gets REJECT, calls retry, and either
  succeeds (channel send fires with retry text) or fails (channel
  send NEVER fires).
- Asserts the DPO negative pair was captured in both branches.

## Migration order

Strict order — each step independently shippable:

1. **Land the inputs struct + the `_ex` function.** Pure addition;
   existing callers untouched. Tests: `test_init_proposer_compose.c`.

2. **Move the validator chain into init_proposer's send wrap.**
   Existing `hu_init_proposer_tick_with_provider_ex` callers (none yet)
   get the chain for free. Tests:
   `test_proactive_dispatch_validator_chain.c`.

3. **Add the `use_unified_dispatch` config flag** (default false).
   Tests: config-parse pin + behavior pin (flag off = old path, on =
   new path).

4. **Wire daemon_proactive's scheduler loop** to call the `_ex`
   function when the flag is on. Tests:
   `test_proactive_dispatch_unified.c`.

5. **A/B observation period (1 week).** No code change; operator
   monitors `~/.human/logs/service-loop-error.log` for FIRED rate
   parity. The retry-outcome telemetry from Sprint 41 follow-up #3
   gives the comparison metric.

6. **Flip default to true.** One-line config change.

7. **Delete the legacy path.** Remove
   `hu_daemon_proactive_prompt_for_contact`'s direct invocation from
   daemon.c; keep the context-builder helper (it now feeds the
   compose-inputs struct).

Steps 1-4 are ~3 days of engineering. Steps 5-7 are calendar time.

## Why this isn't done as part of Sprint 41

- Each migration step needs its own commit + verification.
- The legacy path is currently load-bearing; touching it during the
  Sprint 41 hot-fix would have entangled "stop sending tbh-morning"
  with "rebuild the dispatch architecture". Bad blast radius.
- The A/B period requires real production data — can't be compressed
  into one session.
- The four new test files are ~600 LOC + the implementation is ~400
  LOC across init_proposer, daemon, config_parse. Multi-day work
  belongs in a multi-day sprint.

The Sprint 41 gate-stack unification was the **prerequisite** — both
paths now honor the same gates. This sprint is the dispatch
unification on top.
