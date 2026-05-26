/* src/providers/mlx_stream_utf8.c
 *
 * Sprint 55 US-M3-B4 (Phase 2) — UTF-8 chunk-emission helpers
 * extracted from src/providers/mlx.c so they can be tested without
 * the (gated) subprocess driver.
 *
 * Contract pinned by tests/test_mlx_stream_utf8.c.
 */

#include "human/providers/mlx_stream_utf8.h"

#include <string.h>

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

size_t hu_mlx_utf8_carry_emit(char *carry, size_t *carry_len, size_t carry_cap, const char *content,
                              size_t content_len, char *emit_buf, size_t emit_buf_cap) {
    /* Defensive contract — see header. NULL/zero-cap means fail-open:
     * just copy `content` into `emit_buf` without carry processing. */
    if (!emit_buf || emit_buf_cap == 0)
        return 0;
    if (!content || content_len == 0) {
        /* No new content — emit any existing carry as-is? No: carry is
         * by definition incomplete, so we hold it for the next call.
         * Caller fires no chunk this round. */
        return 0;
    }

    size_t pre_carry = (carry && carry_len) ? *carry_len : 0;
    if (pre_carry > carry_cap)
        pre_carry = carry_cap; /* defensive clamp */

    /* Pathological: (carry + content) won't fit emit_buf. Fall through
     * to fail-open — copy content directly, drop carry-stitch logic. */
    if (pre_carry + content_len > emit_buf_cap) {
        size_t copy_n = content_len < emit_buf_cap ? content_len : emit_buf_cap;
        memcpy(emit_buf, content, copy_n);
        if (carry && carry_len)
            *carry_len = 0;
        return copy_n;
    }

    /* Build [carry][content] in emit_buf */
    if (pre_carry > 0)
        memcpy(emit_buf, carry, pre_carry);
    memcpy(emit_buf + pre_carry, content, content_len);
    size_t total = pre_carry + content_len;

    /* Compute safe-emit boundary */
    size_t safe_n = hu_mlx_utf8_safe_emit_len(emit_buf, total);
    size_t new_carry = total - safe_n;

    /* Stash trailing partial bytes for next call. Carry can be at most 3
     * bytes (lead + 2 continuations of a 4-byte codepoint). If the helper
     * somehow reports a larger carry, clamp to fail-open. */
    if (carry && carry_len) {
        if (new_carry > carry_cap) {
            /* Pathological — emit everything; consumer deals with it */
            safe_n = total;
            new_carry = 0;
        }
        if (new_carry > 0)
            memcpy(carry, emit_buf + safe_n, new_carry);
        *carry_len = new_carry;
    } else {
        /* Caller didn't pass carry state — emit everything */
        safe_n = total;
    }
    return safe_n;
}
