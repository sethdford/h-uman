# M3 B4 `mlx_local` SSE — Design

## Topology

```
[caller channel]                    [hu_mlx_local provider]                 [mlx_server.py]
     │
     │  hu_provider_stream_chat
     ▼
+--------------------+  POST /v1/chat/completions    +-----------------------+
| mlx_local vtable   |  body: {stream:true, ...}     |                       |
| ctx: alloc, base   | ──────────────────────────►   | stream tokens via     |
| url, last latency  |                               | SSE: data: {delta:..} |
+--------------------+ ◄──────────────────────────── |     data: [DONE]      |
       │ (per event)                                 +-----------------------+
       │ parse event → delta.content
       │ UTF-8 boundary buffer
       ▼
+--------------------+
| hu_stream_callback |
| (caller-provided)  |
+--------------------+
```

## New / changed surface

| Symbol | Where | Purpose |
|---|---|---|
| `hu_mlx_local_stream_chat` | `src/providers/mlx_local.c` (existing file, new function) | The `stream_chat` vtable entry. Posts with `stream:true`, parses SSE, fires callback per delta. |
| `hu_mlx_local_supports_streaming` | same file | Returns true when `cfg.mlx_local.streaming_enabled` AND a recent successful SSE round-trip exists (cached on the ctx). |
| `hu_sse_parser_t` | new `src/util/sse_parser.{c,h}` | Pure SSE-event accumulator. Pushes raw bytes in, emits `data:` events out. Reusable for any future SSE consumer (no MLX-specific knowledge). |
| `hu_mlx_local_ctx_t::first_token_latency_ms` | extends existing ctx | Operator-readable signal for AC-B4-10. |
| `cfg.mlx_local.streaming_enabled` | `include/human/config.h` | Default `true`. Operator opt-out for diagnostic purposes. |
| `cfg.mlx_local.first_token_budget_ms` | same | Default 500. Used by AC-B4-10 logging threshold (warn if exceeded). |

## Decisions

### D1 — REUSE existing `hu_sse_parser_t` from `src/providers/sse.c`

**Chose**: Use the already-shipped `hu_sse_parser_t` at
`include/human/providers/sse.h` + `src/providers/sse.c`. It exposes
`hu_sse_parser_init / _feed / _deinit` with a callback-per-event
shape, already used by `src/providers/anthropic.c` and pinned by
`tests/test_streaming.c`.

**Over**: Writing a parallel utility under `src/util/` with a pop-event
API. (Earlier draft of this design proposed exactly that — discarded
2026-05-26 after a duplicate-symbol link error revealed the existing
parser. Lesson logged: grep for `hu_sse_*` BEFORE writing 600 lines
of parser; per `~/.claude/rules/audit-verify-before-allege.md`.)

**Because**: The existing parser is mature, callback-shaped (which
fits libcurl's write-callback model better than pop semantics for
the `mlx_local` consumer), and already linked into the test binary.
Re-using it cuts T1 from "~5 hr build + test" to "~30 min audit",
and the savings flow into T4 (the actual mlx_local consumer).

**Caveat for future use**: the existing parser's header lives at
`include/human/providers/sse.h` — semantically misleading since SSE
is protocol-agnostic. If a future caller needs SSE outside the
provider/ namespace (e.g. a generic event-source consumer), the
right move is to MOVE the header to `include/human/util/sse.h` and
update the four existing call sites (anthropic.c, mlx_local.c after
T4, test_streaming.c, test_provider.c). That refactor is OUT of
scope for B4 — flag it as a one-line follow-up.

### D2 — Reuse existing UTF-8 boundary helpers (Sprint 55 `e86a08a2`)

**Chose**: Wire `hu_mlx_local_stream_chat`'s callback dispatch through
the existing `src/providers/mlx_stream_utf8.c` helpers.

**Over**: Duplicating the boundary logic.

**Because**: Sprint 55 already paid the cost of getting partial-
codepoint handling right (16 unit tests pin it). Forking it would
re-introduce a known bug class. Per `~/.claude/rules/audit-verify-
before-allege.md`, a known-correct shared helper beats a new
re-implementation.

### D3 — SSE-capability detection via response sniff, not pre-flight

**Chose**: Issue the POST with `stream: true` + `Accept: text/event-
stream` unconditionally. If the response `Content-Type` comes back as
`application/json` (instead of `text/event-stream`), the server
doesn't support SSE — fall back to buffered parse of the same
response body. Cache "this server doesn't speak SSE" on the ctx so we
don't repeatedly negotiate.

**Over**: Pre-flight `OPTIONS` call, or `GET /v1/models` version sniff.

**Because**: One round-trip vs two. The sniff is cheap (Content-Type
header arrives in the first packet) and the fallback path is
ALREADY correct (today's only path). The server-side rejection of
`stream: true` becomes a no-op rather than a failure.

### D4 — Cancellation via `curl_multi` + write-callback short-circuit

**Chose**: Use `curl_multi_perform` so the streaming write-callback can
return `CURLE_ABORTED_BY_CALLBACK` to terminate the connection
immediately. The cancellation surface is the SAME signal the
caller's `hu_stream_callback_t` already uses to stop generation.

**Over**: Separate cancel API; per-connection thread + signal handle.

**Because**: Reuses existing libcurl plumbing and the existing caller
contract. No new threading. The 100 ms budget in AC-B4-5 is well
within libcurl's `select` poll cadence.

### D5 — Mock HTTP server in tests (not a real `mlx_server.py`)

**Chose**: Tests stand up an in-process HTTP server (existing pattern
or new helper) that emits pre-canned SSE responses. No real Python
subprocess, no real MLX.

**Over**: Real `mlx_server.py` subprocess.

**Because**: Deterministic, fast, runs in every CI variant including
no-Apple-Silicon. The SSE format is portable; the assertions test the
C client, not the Python server.

### D6 — `cfg.mlx_local.streaming_enabled` default true

**Chose**: New config bool defaults to `true`.

**Over**: Default false (opt-in).

**Because**: The fallback path is correct; opt-in would mean the
latency win never reaches anyone who doesn't read the changelog.
Operator can opt OUT via the doctor-visible flag if they need to
diagnose a streaming-specific regression. Per the silent-config-gated
rule, the disabled path emits one log line.

## Test plan

| Test | Pins |
|---|---|
| `test_sse_parser_emits_single_event_per_data_line` | D1 — pure parser |
| `test_sse_parser_handles_multi_data_line_event` | SSE spec compliance |
| `test_sse_parser_ignores_comment_lines` | SSE spec compliance |
| `test_sse_parser_drops_partial_event_at_buffer_edge` | Defensive accumulator |
| `test_mlx_local_stream_chat_chunks_arrive_in_order` (AC-B4-8) | End-to-end against mock server |
| `test_mlx_local_stream_chat_utf8_boundary_safety` (AC-B4-8) | UTF-8 reuse correctness |
| `test_mlx_local_stream_chat_callback_stop_terminates_connection` (AC-B4-5) | Cancellation |
| `test_mlx_local_stream_chat_falls_back_when_content_type_json` (AC-B4-6) | Server-version fallback |
| `test_mlx_local_supports_streaming_reflects_config_gate` (AC-B4-1) | Config plumbing |
| `test_mlx_local_first_token_latency_logged_when_streaming_live` (AC-B4-10) | Observability |

All tests must pass in every CI variant including HU_ENABLE_MLX=OFF
(the mock-server tests are MLX-agnostic; the `mlx_local` provider is
HTTP, not MLX-runtime-coupled).

## Risks

- **libcurl `curl_multi` complexity** — write-callback semantics are
  finicky. Mitigation: borrow the existing `lora_nightly.c` HTTP
  helper pattern; don't reinvent.
- **SSE buffer leaks** — partial events stay in the accumulator across
  reads; if the stream terminates abruptly the partial buffer must be
  freed. Mitigation: tie buffer lifetime to the SSE parser's deinit;
  ASan catches.
- **Mid-flight UTF-8 split crossing AN SSE event boundary** — payload
  bytes from event N+1 are needed to complete a codepoint from event N.
  Mitigation: the UTF-8 boundary helper already handles cross-call
  state (Sprint 55 proof).
- **Cancellation race** — caller returns stop, write-callback returns
  abort, but server has already pushed more bytes into the curl
  buffer. Those bytes get discarded (correct behavior) but the metric
  for `last_chunk_at` should not advance. Mitigation: gate metric
  update on "did we fire the callback for this byte range" not "did
  we receive bytes."

## Out-of-scope reaffirmed

- Doctor section for `mlx_local` SSE health — separate spec (will reuse
  the doctor pattern from `docs/plans/2026-05-25-doctor-prompt-budget-
  initiative/`).
- Reconnect-on-drop semantics.
- Per-channel streaming opt-in matrix.
