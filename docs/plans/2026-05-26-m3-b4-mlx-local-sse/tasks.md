# M3 B4 `mlx_local` SSE — Tasks

Ordered for incremental shipping. Each task is a single PR; the suite
must stay green between PRs.

## T1 — SSE parser utility (covers D1, half of AC-B4-3/AC-B4-4 testability)

- Create `include/human/util/sse_parser.h` exposing:
  - `hu_sse_parser_t` opaque
  - `hu_sse_parser_init(hu_allocator_t *, hu_sse_parser_t **)`
  - `hu_sse_parser_free(hu_sse_parser_t *)`
  - `hu_sse_parser_push(parser, const char *bytes, size_t n)` — feed
    raw bytes; internal accumulator keeps partial events
  - `hu_sse_parser_pop_event(parser, char **out_data, size_t *out_len)` —
    returns HU_OK if a complete event is available (caller owns out),
    HU_ERR_NOT_FOUND if not enough bytes yet
- Create `src/util/sse_parser.c` with the implementation.
- Add `tests/test_sse_parser.c` with the four parser-unit tests from
  the design's test plan.
- Register in CMakeLists.txt (always-compiled — no MLX dep).
- **Done when**: 4 new tests pass, full suite green.
- **Est**: 4-6 hr.

## T2 — Config plumbing (covers AC-B4-1, AC-B4-10 config side)

- Add to `include/human/config.h`:
  ```c
  typedef struct hu_mlx_local_config {
      bool streaming_enabled;       /* default true */
      int first_token_budget_ms;    /* default 500 */
  } hu_mlx_local_config_t;
  ```
  Embed as `cfg->mlx_local`.
- Add config-parse + config-validate + config-merge entries.
- Add silent-config-gated warning when `streaming_enabled=false` per
  `~/.claude/rules/silent-config-gated-subsystems.md`.
- Add `tests/test_config_mlx_local.c` with parse/merge/default tests.
- **Done when**: 3+ config tests pass, defaults match design.
- **Est**: 2-3 hr.

## T3 — `mlx_local` provider `supports_streaming` + capability cache (covers AC-B4-1, AC-B4-6)

- Add `hu_mlx_local_supports_streaming` vtable entry.
- Add `last_negotiation_status` field on the provider ctx (3-valued:
  unknown / sse / buffered-only).
- Returns `true` only when (a) `cfg.mlx_local.streaming_enabled` AND
  (b) `last_negotiation_status != buffered-only`.
- Add `test_mlx_local_supports_streaming_reflects_config_gate`.
- **Done when**: provider vtable advertises streaming honestly.
- **Est**: 2 hr.

## T4 — `mlx_local_stream_chat` skeleton + mock server harness (covers AC-B4-2, AC-B4-3)

- Build a mock HTTP server helper (or borrow existing) that emits SSE
  events on demand.
- Implement `hu_mlx_local_stream_chat`:
  - POST with `stream: true` + `Accept: text/event-stream`
  - libcurl write-callback feeds bytes into `hu_sse_parser_t`
  - Per popped event, parse `delta.content` JSON and fire caller's
    `hu_stream_callback_t`
  - Drain to `[DONE]` event
- Add `test_mlx_local_stream_chat_chunks_arrive_in_order`.
- **Done when**: 5-chunk stream arrives in order through the callback.
- **Est**: 8-10 hr (this is the bulk of the work).

## T5 — UTF-8 boundary correctness (covers AC-B4-4)

- Wire the existing `src/providers/mlx_stream_utf8.c` helpers into the
  callback dispatch.
- Add `test_mlx_local_stream_chat_utf8_boundary_safety` — mock emits
  a 4-byte emoji split across two events.
- **Done when**: callback never sees a partial codepoint, full emoji
  arrives intact (possibly delayed by 1 event).
- **Est**: 2-3 hr (helpers exist; integration + test).

## T6 — Cancellation via callback-stop (covers AC-B4-5)

- Hook the existing "stop" signal from `hu_stream_chat_result_t` into
  the write-callback so it returns `CURLE_ABORTED_BY_CALLBACK` when
  the caller wants out.
- Confirm libcurl tears down the connection within budget.
- Add `test_mlx_local_stream_chat_callback_stop_terminates_connection`.
- **Done when**: cancellation completes within 100 ms; no leaked
  connections (ASan + valgrind clean).
- **Est**: 3-4 hr.

## T7 — Buffered fallback on non-SSE servers (covers AC-B4-6)

- Sniff response `Content-Type`. If `application/json`, parse as
  one-shot response and fire single callback with full body.
- Cache the fact on ctx so subsequent calls skip the SSE attempt.
- Emit one-shot `hu_log_info_once` with the server version hint.
- Add `test_mlx_local_stream_chat_falls_back_when_content_type_json`.
- **Done when**: same `stream_chat` API works against pre-SSE servers
  with one extra log line and no callback contract change.
- **Est**: 3 hr.

## T8 — Observability: first-token latency metric (covers AC-B4-10)

- Record `clock_gettime` at POST send, at first callback fire.
- Stash delta on ctx as `first_token_latency_ms`.
- Log warn-once when delta > `cfg.mlx_local.first_token_budget_ms`.
- Add `test_mlx_local_first_token_latency_logged_when_streaming_live`
  using a mock server that delays first event.
- **Done when**: latency field populated and reachable via getter for
  the doctor spec.
- **Est**: 2 hr.

## T9 — Doctor section (deferred to a separate doctor spec)

Out of scope for this multi-PR unit. The doctor check
`human doctor mlx_local` belongs in the doctor-section spec at
`docs/plans/2026-05-25-doctor-prompt-budget-initiative/` (extend the
existing pattern). Reference this spec's AC-B4-7 from there.

## T10 — Plan-doc refresh + CLAUDE.md M3 row update

- Update `docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md` to
  mark B4-SSE as SHIPPED.
- Update `CLAUDE.md` M3 row to "Bridge B production-streaming complete."
- **Done when**: docs reflect the new state.
- **Est**: 30 min.

## Sequencing

Strict dependencies: T1 → T4. T2 → T3. T4 → T5, T6, T7, T8 (parallelizable
after T4 lands). T10 happens last.

Total estimated effort: ~26-35 hr — one developer, ~1 week elapsed
with normal SDLC overhead.

## Out of scope reaffirmed

- `mlx_server.py` server-side work
- Subprocess provider streaming (already shipped in Sprint 55)
- Doctor surface (separate spec)
- Reconnect-on-drop
- Per-channel opt-in matrix
