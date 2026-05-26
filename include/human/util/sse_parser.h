/* include/human/util/sse_parser.h
 *
 * Pure Server-Sent Events (SSE) accumulator. First brick of the M3
 * Bridge B Phase B4 work (see docs/plans/2026-05-26-m3-b4-mlx-local-sse/).
 *
 * The parser is intentionally protocol-agnostic — it accepts raw bytes
 * from any source (libcurl write callback, mock test server, file
 * replay) and emits whole `data:` events. Callers stack any JSON /
 * OpenAI / Anthropic schema parsing on top.
 *
 * SSE format (per WHATWG live spec, simplified for the subset we need):
 *
 *   data: <payload-line-1>
 *   data: <payload-line-2>          ← multi-line data: concatenated
 *   <blank line>                    ← terminates one event
 *
 *   : <comment>                     ← ignored
 *   event: <type>                   ← optional, ignored (we only need data)
 *   id: <id>                        ← optional, ignored
 *   retry: <ms>                     ← optional, ignored
 *
 * The terminator is a single blank line (either "\n\n" or "\r\n\r\n").
 * The parser owns a growing accumulator buffer; bytes pushed in are
 * scanned for whole events on each push or pop. Partial events stay
 * buffered until the next push completes them.
 *
 * Memory: hu_sse_parser_init allocates a fixed initial capacity that
 * grows geometrically up to a hard cap. hu_sse_parser_pop_event hands
 * the caller a heap-owned copy of the concatenated `data:` payload
 * (caller must free with the same allocator used for init).
 *
 * Reuse: this header is part of the always-compiled core (no MLX,
 * libcurl, or feature-flag dependency). A future Claude API streaming
 * provider, Anthropic-side dashboard event-source consumer, or any
 * other SSE client can use the same parser unmodified.
 */

#ifndef HU_UTIL_SSE_PARSER_H
#define HU_UTIL_SSE_PARSER_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque parser state. */
typedef struct hu_sse_parser hu_sse_parser_t;

/* Construct an empty parser. The allocator is borrowed; the caller
 * must keep it alive for the parser's lifetime. */
hu_error_t hu_sse_parser_init(hu_allocator_t *alloc, hu_sse_parser_t **out);

/* Free the parser and any internal buffers. Safe on NULL. */
void hu_sse_parser_free(hu_sse_parser_t *p);

/* Feed `n` bytes from `bytes` into the accumulator. Bytes may be a
 * partial event, multiple events, a complete event followed by a
 * partial one, etc. Returns HU_OK on success, HU_ERR_OUT_OF_MEMORY
 * if the accumulator can't grow (currently capped at 64 KiB per
 * event, defensive against pathological input). */
hu_error_t hu_sse_parser_push(hu_sse_parser_t *p, const char *bytes, size_t n);

/* Pop the next complete event off the front of the accumulator.
 *
 * On success (HU_OK):
 *   *out_data     — newly-allocated NUL-terminated payload (the
 *                   concatenated `data:` lines, joined by '\n').
 *                   Caller frees with the parser's allocator.
 *   *out_data_len — length excluding the terminator
 *
 * Returns:
 *   HU_OK           — one event was popped
 *   HU_ERR_NOT_FOUND — no complete event available yet (push more bytes)
 *   HU_ERR_INVALID_ARGUMENT — NULL args
 *   HU_ERR_OUT_OF_MEMORY    — allocation for output failed
 *
 * Comment lines (starting with ':') are silently skipped during scan.
 * Non-`data:` fields (event:, id:, retry:) are parsed-and-ignored;
 * they consume the event slot but produce no output. The "[DONE]"
 * sentinel commonly used by OpenAI-streaming-compatible servers is
 * returned as a normal event with payload exactly "[DONE]" — caller
 * decides whether to treat it as end-of-stream. */
hu_error_t hu_sse_parser_pop_event(hu_sse_parser_t *p, char **out_data, size_t *out_data_len);

/* Total bytes currently held in the accumulator (for diagnostics /
 * back-pressure decisions). Includes both complete-but-not-yet-popped
 * events and the trailing partial event. */
size_t hu_sse_parser_buffered_bytes(const hu_sse_parser_t *p);

#ifdef __cplusplus
}
#endif

#endif /* HU_UTIL_SSE_PARSER_H */
