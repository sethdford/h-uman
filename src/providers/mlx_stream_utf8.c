/* src/providers/mlx_stream_utf8.c
 *
 * Sprint 55 US-M3-B4 (Phase 2) — UTF-8 chunk-emission helpers
 * extracted from src/providers/mlx.c so they can be tested without
 * the (gated) subprocess driver.
 *
 * Contract pinned by tests/test_mlx_stream_utf8.c.
 */

#include "human/providers/mlx_stream_utf8.h"

size_t hu_mlx_utf8_codepoint_len(unsigned char first) {
    if ((first & 0x80) == 0)
        return 1; /* ASCII */
    if ((first & 0xE0) == 0xC0)
        return 2;
    if ((first & 0xF0) == 0xE0)
        return 3;
    if ((first & 0xF8) == 0xF0)
        return 4;
    return 1; /* malformed lead — treat as 1 to avoid stalling */
}

size_t hu_mlx_utf8_safe_emit_len(const char *buf, size_t len) {
    if (!buf || len == 0)
        return 0;
    /* Walk back at most 3 bytes looking for the start of a codepoint
     * that isn't fully completed within `len`. A 4-byte codepoint at
     * the very tail (one lead + three pending continuations) is the
     * deepest "incomplete" case we need to detect. */
    size_t back = (len < 4) ? len : 4;
    for (size_t i = 0; i < back; i++) {
        size_t pos = len - 1 - i;
        unsigned char b = (unsigned char)buf[pos];
        if ((b & 0x80) == 0) {
            /* ASCII byte — safe to emit through here */
            return len;
        }
        if ((b & 0xC0) == 0xC0) {
            /* Lead byte of a multi-byte codepoint */
            size_t cp_len = hu_mlx_utf8_codepoint_len(b);
            size_t remaining = len - pos;
            if (remaining < cp_len) {
                /* Incomplete codepoint at tail — emit up to `pos` only */
                return pos;
            }
            /* Codepoint complete; safe to emit through `len` */
            return len;
        }
        /* Continuation byte (0b10xxxxxx) — keep walking back */
    }
    /* Walked back the max; treat as safe (defensive — malformed input) */
    return len;
}
