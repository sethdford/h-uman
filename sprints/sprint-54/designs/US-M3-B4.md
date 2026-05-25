# Design for US-M3-B4: MLX Streaming Wire

## Approach

Add streaming token-by-token inference to the MLX provider by adapting the existing batch `mlx_run_subprocess` pattern (fork+pipe+exec) to use non-blocking I/O instead of blocking read-until-EOF. The subprocess runs `python3 -m mlx_lm.generate` with unbuffered output (`-u` flag) to minimize latency between token generation and emission. A select()-based read loop with ~100ms timeout collects complete tokens from the stdout pipe, buffers partial UTF-8 sequences, and delivers complete tokens to the caller via callback. Cancellation uses SIGTERM + drain + waitpid, mirroring the shutdown protocol already proven in batch mode.

The design reuses the proven fork+pipe infrastructure rather than introducing a new IPC mechanism (e.g., sockets or gRPC), keeping the change localized to MLX and reducing cross-cutting risk. Reference implementation strategy: examine `src/providers/llamacpp.c` for streaming patterns (token boundary detection, UTF-8 handling, cancellation flow) but NOT copy code — the MVP is conservative (whitespace-delimited tokens) rather than sophisticated token parsing.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `include/human/providers/provider.h` | Verify `stream_chat` vtable signature exists (read-only) | 0 |
| `src/providers/mlx.c` | Add `mlx_stream_chat()` implementation (fork+pipe+select loop) + `mlx_supports_streaming()` | +180 |
| `src/providers/mlx.c` | Wire `.stream_chat = mlx_stream_chat` + `.supports_streaming = mlx_supports_streaming_true` in vtable | +2 |
| `tests/test_mlx_provider.c` | Add `test_mlx_stream_chat_chunks_equal_batch` (verify streaming reconstruction matches batch output) | +50 |
| `tests/test_mlx_provider.c` | Add `test_mlx_stream_chat_cancellation_terminates_subprocess` (verify SIGTERM+waitpid flow) | +40 |

**Total new code: ~272 LOC**

## Implementation steps (for the implementer)

1. **Read the existing patterns**: examine `src/providers/mlx.c::mlx_run_subprocess` (lines 99-248) to understand the fork+pipe+exec shape, and skim `src/providers/llamacpp.c` for streaming token-boundary logic (10–15 min, read-only).

2. **Skeleton `mlx_stream_chat()` function**: create the function signature matching the vtable, add a comment block explaining the three phases (fork, read loop, cleanup), stub all branches. No behavior yet.

3. **Implement fork+exec phase**: reuse `mlx_run_subprocess` internals (process spawn, environment setup) but stop BEFORE the blocking read. Pass `-u` to `python3` for unbuffered output. Set stdout pipe to non-blocking via `fcntl(STDOUT_PIPE_FD, F_SETFL, O_NONBLOCK)`.

4. **Implement select()+read loop**: 
   - Allocate a line buffer (~4 KB)
   - Allocate a UTF-8 hold buffer for partial sequences (~4 bytes)
   - Loop with `select(nfds, &readfds, NULL, NULL, &timeout)` timeout = 100ms
   - On readable: `read()` into line buffer, handle EAGAIN/EINTR
   - On EOF: drain, emit final token, break
   - On timeout: check cancellation flag, yield to caller, loop
   - Buffer incomplete tokens; emit complete whitespace-delimited tokens via callback

5. **Implement cancellation**: check a `*cancel_flag` passed through `callback_ctx` (most common h-uman pattern; verify against `src/providers/llamacpp.c` if needed). On cancel: `kill(pid, SIGTERM)`, drain loop once more, `waitpid(pid, &status, 0)`, return error.

6. **Implement cleanup**: every exit path (success, error, cancellation) must call `waitpid()` to prevent zombies. Use an early-exit guard if reachable from multiple branches.

7. **Wire vtable**: add `.stream_chat = mlx_stream_chat` and `.supports_streaming = mlx_supports_streaming_true` (or create the predicate if it doesn't exist) to `mlx_vtable` in `src/providers/mlx.c`.

8. **Test 1 — equivalence**: `test_mlx_stream_chat_chunks_equal_batch` — make the same inference request via both batch and stream paths, concatenate streamed chunks, assert byte-for-byte equality. Gate on `HU_ENABLE_MLX_PROVIDER && __APPLE__ && __arm64__`; emit `HU_TEST_SKIP("mlx streaming only on Apple Silicon")` on other platforms.

9. **Test 2 — cancellation**: `test_mlx_stream_chat_cancellation_terminates_subprocess` — set up a streaming request with a callback that sets cancel-flag after first token, then verify waitpid succeeds and does not block. Same gating as Test 1.

10. **Verify**: run `./build/human_tests --filter=mlx_stream` and the full suite locally. Commit.

## Risks

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| **Python unbuffered output flag confusion** | Medium | Medium | Test will catch it immediately (first streaming run will hang or emit garbled chunks). Add a comment citing `python3 -u` docs. |
| **Select timeout too long (adds latency) or too short (CPU spin)** | Low | Small | 100ms is conservative — start there, tune down post-MVP if latency budget requires. Implementer can adjust if profiling shows jitter. |
| **UTF-8 boundary mishandling (emit incomplete multibyte char)** | Low | Medium | Buffer partial sequences in a 4-byte hold buffer; only emit when next read completes the sequence. Test with a non-ASCII emoji or CJK prompt. |
| **Whitespace token boundary too conservative (splits mid-word)** | Low | Small | Intended — MVP trades some token fragmentation for simplicity. Caller reassembles via callbacks. Future: consume llamacpp's token parsing if jitter budget allows. |
| **Zombie process on unplanned exit** | Medium | Large | Wrap cleanup in a guard function called from every branch. Test for zombie via `ps` in the cancellation test (implementer knows the pattern from existing C code). |
| **Test skip macro missing** | Low | Small | Grep `tests/test_framework.h` for `HU_TEST_SKIP` before writing; it's a standard pattern in this codebase. If absent, add it (3 lines) to the test framework. |
| **Backpressure handling (callback slower than generation)** | Low | Small | `select()` read loop will naturally throttle — if stdout pipe buffer fills, `read()` blocks. No explicit backpressure needed for MVP. |

## Test strategy

- **Unit scope**: `test_mlx_stream_chat_chunks_equal_batch` exercises the entire function (fork, select loop, token emission) in a single test. No need for granular sub-tests.
- **Platform gate**: both tests skip cleanly on non-Apple-Silicon CI via `HU_TEST_SKIP`. Verify `CMakeLists.txt` still lists these tests in `HU_TEST_SOURCES` unconditionally (no gate needed at register level); the gate fires at runtime.
- **Integration scope**: no cross-provider integration testing in this design. The provider vtable change is mechanical (add two fields to struct); no other provider code changes.

## Acceptance criteria mapping

- **AC 1: `stream_chat` wired in mlx vtable** → verified by `test_mlx_stream_chat_chunks_equal_batch` successfully calling the function (if not wired, test fails with link error or vtable NULL dereference)
- **AC 2: Tokens emitted incrementally via callback as they arrive** → verified by test that asserts callbacks fire multiple times during one request (implementer will instrument the test callback to count invocations)
- **AC 3: Complete reconstruction from streamed chunks matches batch output** → core assertion in `test_mlx_stream_chat_chunks_equal_batch`
- **AC 4: Cancellation via SIGTERM unblocks promptly** → verified by `test_mlx_stream_chat_cancellation_terminates_subprocess` timing the wait on first token receipt
- **AC 5: No zombie processes** → same test verifies `waitpid()` succeeds immediately after SIGTERM

## Out of scope

- Token parsing sophistication (UTF-8 awareness is MVP; advanced token boundary detection deferred to Phase C3)
- Backpressure tuning (select loop naturally throttles; no explicit pause/resume signaling)
- Streaming from cloud providers (Gemini, Claude, etc.) — this design is MLX-only
- Fallback to batch when streaming unavailable (caller's job; provider just returns error if streaming fails)
- Metrics/observability (latency histograms, token/sec throughput) — defer to Phase 2c
- Profiling or adaptive token-buffering (start conservative; tune post-MVP)

## Success criteria at implementation close

- Implementer submits PR with `test_mlx_stream_chat_chunks_equal_batch` and `test_mlx_stream_chat_cancellation_terminates_subprocess` both PASSING on Apple Silicon CI
- Full test suite passes (no regressions)
- Zero zombies reported by `ps` in test execution
- Code compiles clean (`-Wall -Wextra -Werror`)
- Commit message references this US-M3-B4 and notes the vtable wire + two new tests
