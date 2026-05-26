# M3 Bridge B Phase B4 — `mlx_local` HTTP path consumes SSE chunks

## Context

The parent plan (`docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md`)
landed B4 Phase 1-3 via Sprint 54 + 55: the **subprocess-based** MLX
provider streams tokens via `mlx_lm.generate` stdout, fanned out through
`hu_chat_response_chunk`. That ships ~109 tok/s on a 26B MoE locally.

What the parent plan flagged but did NOT close: **production daemon
traffic goes through the `mlx_local` HTTP provider (port 8741, talking
to `mlx_server.py`), NOT the subprocess provider.** Today that HTTP
client buffers the full response before returning — operators see a
multi-second wall-clock pause on long completions instead of incremental
tokens.

This spec covers wiring `mlx_local` to consume the `mlx_server.py`
SSE event stream so streaming-capable channels (iMessage typing
indicators, web UI, etc.) get incremental tokens. It's the last
substantive M3-mission gap before "frontier inference is feature-complete
for production."

## User stories

- **As a user typing on iMessage**, I want to see the assistant's reply
  begin streaming back within ~500 ms of send rather than waiting 3-5 s
  for the full thought, so the conversation feels live.
- **As an operator**, I want to know whether the SSE path is actually
  being exercised (via a `human doctor mlx_local` check) so I can
  distinguish "feature wired" from "feature wired and live."
- **As a developer adding a new streaming channel**, I want the
  `mlx_local` provider to honor `stream_chat` the same way `llamacpp`
  does, so I don't have to special-case the provider per channel.
- **As a verifier writing regression tests**, I want a deterministic
  test that proves streaming chunks arrive in the same order their
  bytes appear in the SSE event stream — no reordering, no lost chunks
  at boundary edges, no UTF-8 splits.

## Acceptance criteria

- **AC-B4-1**: `mlx_local` provider's vtable exposes `stream_chat`
  returning `HU_OK` AND `supports_streaming` returning `true` whenever
  the underlying `mlx_server.py` advertises an SSE-capable endpoint
  (e.g. `Accept: text/event-stream` on `POST /v1/chat/completions`).

- **AC-B4-2**: When `stream_chat` is invoked, the provider sends an HTTP
  POST to `<base_url>/chat/completions` with `stream: true` in the body
  AND `Accept: text/event-stream` in the headers, AND parses the response
  as SSE events of the OpenAI-streaming JSON shape (`data: {...}\n\n`,
  terminated by `data: [DONE]\n\n`).

- **AC-B4-3**: Each parsed `delta.content` from an event fires the
  registered `hu_stream_callback_t` exactly once, in event-arrival
  order, with no reordering across the stream.

- **AC-B4-4**: UTF-8 boundary safety — if the SSE event payload splits
  a multi-byte codepoint across two events, the callback receives only
  complete codepoints. Existing helpers in `src/providers/mlx_stream_utf8.c`
  (Sprint 55 commit `e86a08a2`) are the reference implementation;
  reuse them, don't fork.

- **AC-B4-5**: Mid-stream cancellation — when the caller's callback
  returns a "stop" signal (existing `hu_stream_chat_result_t` mechanism),
  the provider closes the HTTP connection cleanly within 100 ms and the
  `mlx_server.py` cancels generation server-side.

- **AC-B4-6**: Streaming fallback — when `mlx_server.py` does not
  support SSE (older versions, or `Accept: text/event-stream` rejected),
  the provider transparently falls back to the existing buffered
  response path, logs one `hu_log_info_once` line naming the version
  gap, and the caller receives the full response in a single chunk.

- **AC-B4-7**: `human doctor mlx_local` (extends the existing doctor
  command — separate spec for the doctor registry entry) reports SSE
  capability: "streaming: live (last chunk T ms ago)" / "streaming:
  buffered (server doesn't advertise SSE)" / "streaming: disabled
  (cfg.mlx_local.streaming_enabled=false)". A separate AC-B4-7-doctor
  in the doctor spec covers the diag-item shape.

- **AC-B4-8**: Two contract tests pin the wire end-to-end:
  - `test_mlx_local_stream_chat_chunks_arrive_in_order` — mocked HTTP
    server that emits 5 SSE events with monotonically-ordered marker
    tokens; assert callback receives them in same order.
  - `test_mlx_local_stream_chat_utf8_boundary_safety` — mock emits an
    event that splits a 4-byte emoji across the byte boundary; assert
    callback never sees a partial codepoint.
  Both tests gate on the same env as B4 Phase 1-3 (Apple Silicon
  optional — the mock HTTP server is portable).

- **AC-B4-9**: No regression in the non-streaming path. All existing
  `mlx_local` tests pass. The buffered code path remains the default
  for callers that pass `NULL` callback.

- **AC-B4-10**: Latency win measured — when streaming is live, first
  callback fires within `cfg.mlx_local.first_token_budget_ms` (default
  500 ms) of the POST. Logged as `mlx_local.first_token_latency_ms` so
  operators can confirm the win after rollout.

## Non-goals

- **No `mlx_server.py` work.** The Python server already advertises SSE
  per the OpenAI-compatible shape. This spec is C-client-side only.
- **No subprocess-provider changes.** B4 Phase 1-3 already shipped for
  the subprocess provider; we don't re-touch it.
- **No new dependencies.** Reuse existing `libcurl` plumbing (already a
  build-time dep). No SSE library — the format is trivial enough to
  parse inline.
- **No client-side reconnect / retry.** If the SSE stream drops mid-
  response, the caller sees an error; reconnection is a separate spec.
- **No SSE on outbound directions other than chat.** `/adapters/swap`,
  `/health`, `/v1/models` stay request-response.
- **No per-channel opt-in matrix.** All streaming channels get SSE
  uniformly; non-streaming channels keep buffered.

## Constraints

- C11 + `-Wall -Wextra -Wpedantic -Werror`. No new warnings.
- `hu_*` naming. No `SQLITE_TRANSIENT`.
- No real network in tests — mock HTTP server pattern from
  `tests/test_http_mock_server.c` (or equivalent existing harness).
- `HU_IS_TEST` guards on any path that would hit `mlx_server.py`.
- Full suite passes (currently 12,200+ tests). 0 ASan errors.
- Pre-commit hooks (`scripts/check-test-source-gate-symmetry.sh`,
  `scripts/check-test-references.sh`) pass.
- `~/.claude/rules/silent-config-gated-subsystems.md` — if SSE is
  disabled in config, emit one operator-visible log line naming the
  config key.

## Open questions for Design phase

- **Buffered fallback signal**: how does the provider detect that
  `mlx_server.py` doesn't support SSE? Sniffing the first response
  byte? A pre-flight `OPTIONS` call? An explicit version pin via
  `cfg.mlx_local.min_server_version`?
- **Callback contract**: existing `hu_stream_callback_t` is shared with
  llamacpp's streaming path — does it already match the OpenAI delta
  shape, or do we need an SSE-specific shim?
- **Cancellation propagation**: closing the HTTP connection client-side
  should make `mlx_server.py` cancel the generation, but only if the
  server is configured to honor the disconnect. Investigate
  `KeepAlive` / `Connection: close` handling.

## Sequencing

This spec is the **final** substantive M3 work. After it ships:

1. M3 mission status moves from "Bridge B daemon-pattern proven" to
   "Bridge B production-streaming complete."
2. Latency-sensitive channels (iMessage, web) feel the user-visible
   payoff.
3. The remaining M3 items (env-var → config plumbing for nightly LoRA,
   live empirical re-validation on Apple Silicon) are small follow-ups.

Estimated effort: ~1 week (Design 1 day + 3-4 days build + 1 day test
hardening). One developer.
