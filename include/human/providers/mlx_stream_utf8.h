/* include/human/providers/mlx_stream_utf8.h
 *
 * Sprint 55 US-M3-B4 (Phase 2) — UTF-8 chunk-emission helpers.
 *
 * The MLX streaming driver in src/providers/mlx.c reads bytes from a
 * Python mlx_lm subprocess into a chunk buffer and emits sub-buffers
 * to the caller's stream callback. Because reads can land mid-
 * codepoint, the emitter must hold any trailing partial UTF-8
 * sequence until the next read completes it — otherwise the callback
 * sees malformed UTF-8 mid-stream.
 *
 * These helpers are the pure-logic core of that policy. They are
 * unit-testable in isolation (no subprocess, no I/O), which is exactly
 * the gap Phase 1 left: the subprocess path is gated by
 * HU_MLX_SUBPROCESS_ACTIVE (off under HU_IS_TEST), so the helpers
 * had no test coverage. Phase 2 hoists them out so the chunk-safety
 * contract is pinned.
 *
 * Used by:
 *   - src/providers/mlx.c (streaming driver — production path)
 *   - tests/test_mlx_stream_utf8.c (Phase 2 contract pin)
 */
#ifndef HU_PROVIDERS_MLX_STREAM_UTF8_H
#define HU_PROVIDERS_MLX_STREAM_UTF8_H

#include <stddef.h>

/* Returns the codepoint length implied by the first byte of a UTF-8
 * sequence: 1 (ASCII), 2, 3, or 4. Malformed leading bytes return 1
 * defensively so the caller can advance without stalling. */
size_t hu_mlx_utf8_codepoint_len(unsigned char first);

/* Returns the length of buf[0..len] safe to emit through the stream
 * callback without splitting a UTF-8 codepoint. If the tail of the
 * buffer contains the start of a multi-byte codepoint whose continuation
 * bytes haven't arrived yet, the returned length is the offset of that
 * lead byte (so the partial sequence stays in the caller's buffer for
 * the next read). For an empty input, returns 0. For a buffer ending
 * on a complete codepoint, returns len unchanged. */
size_t hu_mlx_utf8_safe_emit_len(const char *buf, size_t len);

/* T5 (2026-05-26) — Streaming carry-buffer state-machine.
 *
 * Used by `compatible_stream_chat` to keep UTF-8 codepoints intact
 * across SSE event boundaries. The compatible provider receives one
 * SSE event per token (mlx-server's pacing); if a token is a single
 * byte of a multi-byte codepoint, the trailing partial sequence is
 * stashed in a 4-byte carry buffer until the next event delivers the
 * continuation bytes.
 *
 * Semantics:
 *   - Pre-state: `carry[0..*carry_len]` holds incomplete UTF-8 from
 *     prior calls. `carry_cap` is the carry buffer capacity (must be
 *     >= 4 to hold any valid UTF-8 prefix).
 *   - Input: new `content` bytes from the just-parsed event.
 *   - Output: fills `emit_buf[0..ret]` with bytes safe to fire through
 *     the user callback (= prior_carry + new_content trimmed to a
 *     codepoint boundary). Returns the safe length.
 *   - Side effects: updates `*carry_len` to the new carry length (0
 *     if everything ended on a complete codepoint); writes new carry
 *     bytes to `carry[0..*carry_len]`.
 *
 * Pathological case: if (carry_in + content) exceeds emit_buf_cap, the
 * helper copies content directly into emit_buf un-stitched (no carry
 * update), and returns content_len. Tokens are small (<256 bytes
 * typical); this fallback only fires on hostile payloads and is the
 * "fail open, deliver bytes" choice over dropping data.
 *
 * Pinned by tests/test_mlx_stream_utf8.c (carry_emit_* contract). */
size_t hu_mlx_utf8_carry_emit(char *carry, size_t *carry_len, size_t carry_cap, const char *content,
                              size_t content_len, char *emit_buf, size_t emit_buf_cap);

#endif /* HU_PROVIDERS_MLX_STREAM_UTF8_H */
