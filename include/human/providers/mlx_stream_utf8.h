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

#endif /* HU_PROVIDERS_MLX_STREAM_UTF8_H */
