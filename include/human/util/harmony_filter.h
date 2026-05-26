/* include/human/util/harmony_filter.h
 *
 * Streaming-safe Harmony channel-marker stripper. Companion to the
 * existing non-streaming `strip_harmony` helper inside
 * `src/agent/response_guard.c` — same intent (strip
 * `<|channel>thought` / `<|channel|>thought` / `<|message|>` markers
 * from model output), but accepts bytes incrementally and handles
 * markers split across chunk boundaries.
 *
 * Use case (M3 Bridge B Phase B4 T4): the production daemon's
 * `mlx_local` HTTP path goes through `compatible_stream_chat` which
 * pipes SSE-arriving content chunks to the caller's
 * `hu_stream_callback_t`. mlx-server.py's `strip_thought_channels`
 * postprocessor only runs in the non-streaming response shape, so
 * streaming chunks contain raw Harmony markers. This filter sits
 * between the SSE parser and the user callback; the caller pushes
 * each arriving chunk through `_push`, gets back clean text safe to
 * emit, and calls `_finish` at end-of-stream to drain whatever was
 * being held back as a possible partial marker.
 *
 * Marker grammar (matches the non-streaming `strip_harmony`):
 *   <|TAG|>           — well-formed Harmony tag (e.g. <|channel|>,
 *                       <|message|>, <|thought|>, <|return|>)
 *   <|TAG             — unclosed leak shape; followed by an optional
 *                       '>', optionally followed by one of the known
 *                       channel values ("thought", "final",
 *                       "analysis", "commentary")
 *
 * Hold-back discipline: when a chunk ends with a prefix that COULD
 * become a marker once more bytes arrive (e.g. ends with `<`, `<|`,
 * `<|cha`), those bytes stay in the accumulator until the next push
 * disambiguates. The push call emits all bytes BEFORE the held-back
 * tail. `_finish` flushes whatever's left through one final strip
 * pass — at end-of-stream, an incomplete `<|...` becomes harmless
 * literal text and is emitted as-is.
 *
 * Memory: caller owns the output strings handed back by `_push` and
 * `_finish` (heap-allocated with the parser's allocator). The
 * accumulator grows geometrically up to a hard cap.
 *
 * Reusability: pure C utility, no provider / SSE / HTTP dependency.
 * Lives in `src/util/` so any future streaming consumer can apply
 * the same filter without pulling in compatible.c.
 */

#ifndef HU_UTIL_HARMONY_FILTER_H
#define HU_UTIL_HARMONY_FILTER_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque accumulator state. */
typedef struct hu_harmony_filter hu_harmony_filter_t;

/* Construct an empty filter. The allocator is borrowed; the caller
 * must keep it alive for the filter's lifetime. */
hu_error_t hu_harmony_filter_init(hu_allocator_t *alloc, hu_harmony_filter_t **out);

/* Free the filter and any internal buffers. Safe on NULL. */
void hu_harmony_filter_free(hu_harmony_filter_t *f);

/* Push `n` bytes from `bytes` through the filter. On return:
 *   *out      — newly-allocated NUL-terminated string with all
 *               markers stripped from the safe-to-emit prefix.
 *               Empty string (and out_len=0) is valid when the
 *               entire chunk is held back as a possible partial
 *               marker. Caller frees with the filter's allocator.
 *   *out_len  — length excluding the terminator.
 *
 * Returns:
 *   HU_OK                — bytes processed
 *   HU_ERR_INVALID_ARGUMENT — NULL args
 *   HU_ERR_OUT_OF_MEMORY — accumulator or output alloc failed
 *
 * Bytes that cannot be unambiguously classified yet (because the
 * chunk ends mid-marker) are retained inside the filter and emitted
 * on the next `_push` or final `_finish`. */
hu_error_t hu_harmony_filter_push(hu_harmony_filter_t *f, const char *bytes, size_t n, char **out,
                                  size_t *out_len);

/* End-of-stream drain. Emits whatever's left in the accumulator
 * through one final strip pass. After `_finish` the filter is empty
 * but still usable (callers may reuse it for a new stream by simply
 * pushing again).
 *
 * Same output ownership + error semantics as `_push`. */
hu_error_t hu_harmony_filter_finish(hu_harmony_filter_t *f, char **out, size_t *out_len);

/* Total bytes currently held back as a possible partial marker.
 * Operator/test signal — should be near zero during normal flow,
 * non-zero only at intra-marker chunk boundaries. */
size_t hu_harmony_filter_buffered_bytes(const hu_harmony_filter_t *f);

#ifdef __cplusplus
}
#endif

#endif /* HU_UTIL_HARMONY_FILTER_H */
