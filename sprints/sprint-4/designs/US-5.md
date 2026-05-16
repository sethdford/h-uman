# US-5: Observer telemetry for validator REJECT/REWRITE

## Approach
Keep the chain pure (no observer reference). Telemetry hook lives at **call sites**, immediately after `hu_output_validator_chain_execute` returns. This is consistent with Sprint-3 architecture (chain = pure data transform) and minimizes API churn.

Add a new tag to `hu_observer_event_tag_t` in `include/human/observer.h:10`:

```
HU_OBSERVER_EVENT_VALIDATOR_DECISION,
```

Add a new union arm to `hu_observer_event_t.data` at `observer.h:31-110`:

```
struct {
    const char *validator_name; /* failing validator from cr.failing_validator_id, or "<chain>" */
    const char *decision;       /* "reject" | "rewrite" — PASS suppressed per PO ruling */
    const char *channel_id;
    const char *persona_name;
    size_t response_len;        /* original input length */
    size_t bytes_stripped;      /* (input_len - final_text_len) for REWRITE; 0 for REJECT */
} validator_decision;
```

(All `const char *` are non-owning; caller guarantees lifetime through the synchronous `record_event` call.)

Add a thin helper `hu_observer_emit_validator_decision(hu_observer_t obs, const hu_chain_result_t *cr, const hu_validator_context_t *vctx, size_t input_len)` in a new `src/observability/validator_telemetry.c`. The helper:
- Returns early if `cr->final_decision == HU_VALIDATOR_PASS` (PO ruling: suppress PASS).
- Constructs the event and calls `hu_observer_record_event`.
- No-ops safely if `obs.vtable == NULL` (matches existing `hu_observer_record_event` static inline behavior at `observer.h:139`).

Call sites that emit: every `hu_output_validator_chain_execute` consumer. For Sprint 4 scope, instrument the 3 primary paths — `daemon.c:2096`, `agent/agent_turn.c:5585`, `agent/agent_stream.c:1425+2145`. The other call sites (`channels/format.c`, `imessage.c`, `openai_compat.c`, `daemon_cron.c`) get the helper call too, but observer is `hu_observer_noop()` when no agent context is available — degrades gracefully.

## Files to modify / create
| File | Change | LOC |
|---|---|---|
| `include/human/observer.h:10-110` | new enum tag + union arm | +12 |
| `src/observability/validator_telemetry.{c,h}` (new) | emit helper | +50 |
| `src/daemon.c:~2096`, `src/agent/agent_turn.c:~5585`, `src/agent/agent_stream.c:~1425,~2145` | call helper after chain execute | +20 |
| `tests/test_validator_telemetry.c` (new) | capturing observer, 3 cases | +120 |

## Implementation steps
1. Add the enum tag + union arm. Build (nothing emits yet — existing observers must compile; verify the switch-on-tag sites use `default:`).
2. Write `validator_telemetry.{c,h}` with the no-op-safe helper.
3. Write the test with a capturing observer (vtable that stashes events into a vector).
4. Wire one call site (`agent_turn.c:5585`). Test green for REJECT path.
5. Wire remaining sites. Run full suite + ASan.

## Risks
- **Switch-on-tag exhaustiveness (MED/SMALL)**: adding an enum value may break `-Werror=switch` in existing observer implementations (`log.c`, `metrics.c`, `composite.c`). Mitigation: grep `case HU_OBSERVER_EVENT_` to enumerate switches, add `default:` or explicit case. Pre-flight: `grep -rn "case HU_OBSERVER_EVENT" src/`.
- **String lifetime (MED/MED)**: `cr->failing_validator_id` and `vctx->channel` are caller-owned; emit must complete before caller frees them. Mitigation: emit happens synchronously before `hu_chain_result_free`. Document in helper doxygen.
- **PASS suppression hides throughput (LOW/SMALL)**: PO ruling. If operators need baseline counts later, add a metric (not event) in Sprint 5.
- **Observability (LOW)**: helper itself logs on misuse (e.g., NULL `cr`) via `HU_ERR_INVALID_ARG` return.

## Test strategy
- New `tests/test_validator_telemetry.c`:
  - AC-5.3: REJECT input → event captured with `decision == "reject"`, `validator_name` matches, `response_len == input_len`, `bytes_stripped == 0`.
  - AC-5.4: REWRITE input → `decision == "rewrite"`, `bytes_stripped > 0`.
  - AC-5.5: NULL observer (`hu_observer_noop()`) → no crash, full path executes.
  - Extra: PASS input → no event emitted (assert capture buffer empty).

## AC mapping
- AC-5.1 → enum + union (compile-time check via test that references the tag)
- AC-5.2 → emit-only-on-non-PASS (test case for PASS suppression)
- AC-5.3 → REJECT case
- AC-5.4 → REWRITE case
- AC-5.5 → noop-observer case

## Effort
**S** — one enum addition + 50-LOC helper + 4 call-site instrumentations + new test. ~200 LOC. Risk: enum exhaustiveness audit.
