/* src/agent/prompt_trim.c — value-aware system-prompt trim helpers.
 *
 * See include/human/agent/prompt_trim.h for the contract and
 * docs/research/2026-07-11-prompt-composition-shrink-plan.md for the
 * measurement that motivated this (204 production positional-truncation
 * events deleting the anti-AI-tell guard tail). */
#include "human/agent/prompt_trim.h"

#include <stdlib.h>
#include <string.h>

hu_prompt_trim_mode_t hu_prompt_trim_mode_parse(const char *value) {
    if (!value || !*value)
        return HU_PROMPT_TRIM_OFF;
    if (strcmp(value, "live") == 0 || strcmp(value, "on") == 0 || strcmp(value, "1") == 0)
        return HU_PROMPT_TRIM_LIVE;
    if (strcmp(value, "shadow") == 0)
        return HU_PROMPT_TRIM_SHADOW;
    /* "off" and anything unrecognized fail closed. */
    return HU_PROMPT_TRIM_OFF;
}

hu_prompt_trim_mode_t hu_prompt_trim_mode(void) {
    return hu_prompt_trim_mode_parse(getenv("HU_PROMPT_TRIM"));
}

size_t hu_prompt_trim_plan(const char *buf, size_t len, size_t budget,
                           const hu_prompt_trim_span_t *spans, size_t span_count,
                           size_t *cuts_out) {
    if (cuts_out && span_count > 0)
        memset(cuts_out, 0, span_count * sizeof(*cuts_out));
    if (!buf || !spans || !cuts_out || span_count == 0 || len <= budget)
        return 0;

    size_t needed = len - budget;
    size_t total = 0;
    for (size_t i = 0; i < span_count && needed > 0; i++) {
        size_t off = spans[i].offset;
        size_t avail = spans[i].length;
        if (avail == 0 || off >= len || avail > len - off)
            continue; /* absent or out-of-range span */
        size_t cut = needed < avail ? needed : avail;
        /* Extend a partial head cut forward to the next newline so the
         * surviving section content starts at a line boundary. */
        while (cut < avail && buf[off + cut - 1] != '\n')
            cut++;
        cuts_out[i] = cut;
        total += cut;
        needed = needed > cut ? needed - cut : 0;
    }
    return total;
}

size_t hu_prompt_trim_apply(char *buf, size_t len, const hu_prompt_trim_span_t *spans,
                            size_t span_count, const size_t *cuts) {
    if (!buf || !spans || !cuts || span_count == 0)
        return len;

    /* Remove cut regions highest-offset first so earlier offsets stay
     * valid as the buffer compacts. Spans are few (3); selection sort. */
    size_t order[8];
    if (span_count > sizeof(order) / sizeof(order[0]))
        span_count = sizeof(order) / sizeof(order[0]);
    for (size_t i = 0; i < span_count; i++)
        order[i] = i;
    for (size_t i = 0; i + 1 < span_count; i++)
        for (size_t j = i + 1; j < span_count; j++)
            if (spans[order[j]].offset > spans[order[i]].offset) {
                size_t t = order[i];
                order[i] = order[j];
                order[j] = t;
            }

    for (size_t k = 0; k < span_count; k++) {
        size_t i = order[k];
        size_t off = spans[i].offset;
        size_t cut = cuts[i];
        if (cut == 0 || off >= len)
            continue;
        if (cut > spans[i].length)
            cut = spans[i].length; /* defensive clamp to the span */
        if (cut > len - off)
            cut = len - off;
        memmove(buf + off, buf + off + cut, len - off - cut);
        len -= cut;
    }
    buf[len] = '\0';
    return len;
}
