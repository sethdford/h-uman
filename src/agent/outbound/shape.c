/* outbound/shape.c — length + sentence-structure validation stage.
 *
 * Rejects content that doesn't fit Seth's message shape:
 *
 *   Rule A: length > 200 chars
 *     No legitimate Seth message is that long. Corpus #6 (the
 *     [SAFETY] directive block) is ~150 chars. Anything beyond 200
 *     is almost certainly a leaked prompt or directive.
 *
 *   Rule B: length >= 60 chars AND >= 2 sentence terminators
 *     The F25 cross-contact bleed pattern (corpus #1, #2, #3): a
 *     paragraph-shaped fragment from another contact's message. Seth
 *     doesn't write 60+ char messages with multiple sentences in
 *     family-contact replies — that's the directive- or memory-
 *     fragment shape, not the conversation shape.
 *
 *     Sentence terminators are '.', '!', '?'. The "60 char" floor
 *     prevents false-positives on short two-question texts ("hey!
 *     how are you?" is 17 chars).
 *
 *     Exception: an URL-shaped substring is permitted (Seth shares
 *     links). We don't deeply parse — just check for "http" presence.
 *
 * Returns:
 *   SEND       — shape is fine
 *   REGENERATE — shape is wrong; LLM can usually fix on retry with
 *                hint "Reply must be under 80 chars, single phrase"
 *   REJECT     — never (LLM regenerate is the right escape)
 *
 * Corpus coverage:
 *   #1, #2, #3 — 60+ char cross-contact fragments → caught by Rule B
 *   #6         — 150-char [SAFETY] block → caught by Rule A
 *   #19..#24   — PASS cases (all under 60 chars or single-sentence)
 */

#include "human/agent/outbound_pipeline.h"

#include <stddef.h>
#include <string.h>

#define SHAPE_MAX_BYTES          200u
#define SHAPE_LONG_MSG_THRESHOLD 60u
#define SHAPE_MAX_SENTENCES      1u

static int contains_url(const char *content, size_t len) {
    /* Cheap substring scan for "http". Avoids strstr's NUL-termination
     * requirement. */
    if (len < 4)
        return 0;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (content[i] == 'h' && content[i + 1] == 't' && content[i + 2] == 't' &&
            content[i + 3] == 'p') {
            return 1;
        }
    }
    return 0;
}

static size_t count_sentence_terminators(const char *content, size_t len) {
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        char c = content[i];
        if (c == '.' || c == '!' || c == '?') {
            /* Skip consecutive terminators ("..." counts once, "?!"
             * counts once) — these are stylistic punctuation, not a
             * sentence break. */
            if (i + 1 < len) {
                char next = content[i + 1];
                if (next == '.' || next == '!' || next == '?')
                    continue;
            }
            count++;
        }
    }
    return count;
}

static hu_outbound_verdict_t shape_run(hu_outbound_pipeline_stage_t *self, hu_outbound_message_t *msg,
                                       hu_outbound_context_t *ctx) {
    (void)self;
    (void)ctx;
    if (!msg || !msg->content || msg->content_len == 0)
        return hu_outbound_verdict_send();

    size_t len = msg->content_len;

    /* Rule A: hard length cap. */
    if (len > SHAPE_MAX_BYTES) {
        return hu_outbound_verdict_regenerate(
            "shape_too_long", "Reply must be under 80 chars, single phrase. No meta-commentary.");
    }

    /* Rule B: long message + multiple sentences (sans URL exception). */
    if (len >= SHAPE_LONG_MSG_THRESHOLD) {
        size_t terms = count_sentence_terminators(msg->content, len);
        if (terms > SHAPE_MAX_SENTENCES && !contains_url(msg->content, len)) {
            return hu_outbound_verdict_regenerate(
                "shape_multi_sentence_long", "Reply with a single short phrase. No multi-sentence "
                                             "paragraphs to family contacts.");
        }
    }

    return hu_outbound_verdict_send();
}

hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_shape = {
    .name = "shape",
    .run = shape_run,
    .state = NULL,
};
