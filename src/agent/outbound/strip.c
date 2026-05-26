/* outbound/strip.c — character-normalization stage.
 *
 * Strips problematic codepoints in-place and returns REWRITE if any
 * were removed, SEND otherwise.
 *
 * Codepoints stripped (rationale per codepoint):
 *
 *   U+FFFC OBJECT REPLACEMENT CHARACTER (UTF-8: EF BF BC)
 *     iMessage uses this for inline attachment placeholders. When
 *     chat.db rows with attachments leaked into LLM input → output,
 *     this character ended up in temporal follow-ups. Real corpus
 *     hit: it wasn't in the Annie/Mindy/Betty visible REJECTs but
 *     was in the broader log evidence.
 *
 *   U+202E RIGHT-TO-LEFT OVERRIDE (UTF-8: E2 80 AE)
 *     Reverses subsequent character rendering direction. Classic
 *     "filename spoofing" attack vector. Defensive: not in observed
 *     corpus but cheap to block.
 *
 *   U+200D ZERO WIDTH JOINER (UTF-8: E2 80 8D)
 *     Renders nothing on its own; joins adjacent glyphs into emoji
 *     ZWJ sequences. Outside of legitimate emoji sequences, a stray
 *     ZWJ is steganography or accidental leak from an upstream
 *     prompt-injection vector. We strip ONLY isolated ZWJs (those
 *     NOT sitting between two 4-byte UTF-8 codepoints); legitimate
 *     emoji ZWJ sequences are preserved.
 *
 *   U+200B ZERO WIDTH SPACE (UTF-8: E2 80 8B)
 *     Renders nothing. Used in homoglyph attacks to bypass word-
 *     boundary checks. Defensive.
 *
 * NOT stripped (left intentionally):
 *   - Newlines, CR, tabs (shape stage handles structural issues)
 *   - All other emoji including ZWJ-joined sequences where ZWJ sits
 *     between two emoji codepoints — see is_isolated_zwj()
 *
 * Returns:
 *   SEND     — nothing was stripped
 *   REWRITE  — verdict.replacement holds the stripped content
 *   REJECT   — never (this stage is non-judgmental; it just normalizes)
 */

#include "human/agent/outbound_pipeline.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* UTF-8 byte patterns for the codepoints we strip. All are 3-byte
 * sequences (codepoint range U+0800..U+FFFF). */

static inline int is_u_fffc(const unsigned char *s) {
    return s[0] == 0xEF && s[1] == 0xBF && s[2] == 0xBC;
}

static inline int is_u_202e(const unsigned char *s) {
    return s[0] == 0xE2 && s[1] == 0x80 && s[2] == 0xAE;
}

static inline int is_u_200b(const unsigned char *s) {
    return s[0] == 0xE2 && s[1] == 0x80 && s[2] == 0x8B;
}

static inline int is_u_200d(const unsigned char *s) {
    return s[0] == 0xE2 && s[1] == 0x80 && s[2] == 0x8D;
}

/* ZWJ is LEGITIMATE when joining two emoji-class codepoints. We
 * approximate "emoji-class" as "any 4-byte UTF-8 sequence" (covers
 * the U+1F300..U+1FFFF block where most emoji live). If ZWJ is
 * between two 4-byte sequences we keep it; otherwise strip.
 *
 * `off` is the index of the ZWJ's first byte (the 0xE2) in `buf`.
 */
static int is_isolated_zwj(const unsigned char *buf, size_t buf_len, size_t off) {
    if (off + 3 > buf_len)
        return 1; /* malformed trailing — strip */

    /* Forward: is buf[off+3] the leading byte of a 4-byte codepoint? */
    int forward_4byte = 0;
    if (off + 3 < buf_len) {
        unsigned char next = buf[off + 3];
        forward_4byte = (next >= 0xF0 && next <= 0xF7);
    }

    /* Backward: find start of the previous codepoint. */
    int backward_4byte = 0;
    if (off > 0) {
        size_t i = off;
        while (i > 0 && (buf[i - 1] & 0xC0) == 0x80)
            i--; /* skip continuations */
        if (i > 0)
            i--;
        unsigned char start = buf[i];
        backward_4byte = (start >= 0xF0 && start <= 0xF7);
    }

    return !(forward_4byte && backward_4byte);
}

/* Strip in-place, return new length. w never advances past r so
 * in-place is safe. */
static size_t strip_codepoints(unsigned char *buf, size_t len) {
    size_t r = 0;
    size_t w = 0;
    while (r < len) {
        unsigned char b = buf[r];
        if (r + 3 <= len && (b & 0xF0) == 0xE0) {
            int drop = 0;
            if (is_u_fffc(buf + r) || is_u_202e(buf + r) || is_u_200b(buf + r)) {
                drop = 1;
            } else if (is_u_200d(buf + r) && is_isolated_zwj(buf, len, r)) {
                drop = 1;
            }
            if (drop) {
                r += 3;
                continue;
            }
        }
        if (w != r)
            buf[w] = buf[r];
        r++;
        w++;
    }
    return w;
}

static hu_outbound_verdict_t strip_run(hu_outbound_pipeline_stage_t *self, hu_outbound_message_t *msg,
                                       hu_outbound_context_t *ctx) {
    (void)self;
    if (!msg || !msg->content || msg->content_len == 0)
        return hu_outbound_verdict_send();
    if (!ctx || !ctx->alloc)
        return hu_outbound_verdict_send();

    size_t orig_len = msg->content_len;
    unsigned char *work = (unsigned char *)ctx->alloc->alloc(ctx->alloc->ctx, orig_len + 1);
    if (!work)
        return hu_outbound_verdict_send();
    memcpy(work, msg->content, orig_len);
    work[orig_len] = '\0';

    size_t new_len = strip_codepoints(work, orig_len);
    if (new_len == orig_len) {
        ctx->alloc->free(ctx->alloc->ctx, work, orig_len + 1);
        return hu_outbound_verdict_send();
    }
    work[new_len] = '\0';

    return hu_outbound_verdict_rewrite("strip_codepoints", (char *)work, new_len);
}

hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_strip = {
    .name = "strip",
    .run = strip_run,
    .state = NULL,
};
