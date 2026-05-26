# M3 B4 `mlx_local` SSE — Tasks

Ordered for incremental shipping. Each task is a single PR; the suite
must stay green between PRs.

## T1 — Audit existing parser + WAIT for in-flight rename to settle (REVISED 2026-05-26)

**Updated** — earlier draft of T1 proposed writing a new parser under
`src/util/sse_parser.c`. A parser ALREADY exists at `src/providers/sse.c`
(header `include/human/providers/sse.h`) with a callback-per-event shape
used by `src/providers/anthropic.c`. Tests in `tests/test_streaming.c`
cover multi-event arrival, partial buffers, and `[DONE]` semantics.

**Critical context**: the existing parser is mid-namespace-rename
(uncommitted in the working tree as of 2026-05-26). The type and most
functions were renamed `hu_sse_parser_* → hu_provider_sse_parser_*`,
but `hu_sse_parser_deinit` was missed (declared with new type but
old name). T1 should NOT touch SSE code until that rename has been
committed and reconciled. Trying to land B4 against a half-renamed
namespace risks merge conflicts and re-introducing the same
duplicate-symbol class of bugs.

Audit results (read-only, 2026-05-26):
- ✅ Multi-`data:` concatenation — covered (sse.c:145-170)
- ✅ Comment lines (`:` prefix) ignored — covered (sse.c:140)
- ✅ CRLF terminators — covered (sse.c:111, 118-119, 134, 178)
- ✅ Partial events at buffer edge — covered (sse.c:115-116)
- ✅ `[DONE]` sentinel — covered via line-level helper (sse.c:235-238)
- ✅ Buffer growth — covered with realloc + 256KB cap (sse.c:81-90)
- ✅ Event-type field (`event:`) — covered (sse.c:171-174)
- ⚠️ No first-class `pop_event` style — feed-with-callback only. That's
   fine for libcurl write-callback, slightly awkward for replay/mock.
   Not a blocker for B4.

Revised T1:
- Audit complete ✅ (this section)
- **DO NOT** add code to this area until the in-flight rename lands
- When the rename settles, T4 (the mlx_local consumer) can wire to
  `hu_provider_sse_parser_*` directly — no further T1 work needed
- **Done when**: this audit is documented (it is) AND the
  namespace rename is committed
- **Est**: 0 additional hr (audit shipped here)

**Followup (not in B4)**: the header lives in `include/human/providers/`
but SSE is protocol-agnostic. Moving it to `include/human/util/sse.h`
+ updating 4 call sites is a one-line refactor for a future cleanup
PR — not in scope here. The mid-flight rename to `hu_provider_sse_parser_*`
actually moves AWAY from the eventual cross-namespace utility status.

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
