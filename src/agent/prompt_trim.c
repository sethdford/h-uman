/* src/agent/prompt_trim.c — value-aware system-prompt trim helpers.
 *
 * See include/human/agent/prompt_trim.h for the contract and
 * docs/research/2026-07-11-prompt-composition-shrink-plan.md for the
 * measurement that motivated this (204 production positional-truncation
 * events deleting the anti-AI-tell guard tail). */
#include "human/agent/prompt_trim.h"

#include "human/core/gate_mode.h"

#include <stdlib.h>
#include <string.h>

hu_prompt_trim_mode_t hu_prompt_trim_mode_parse(const char *value) {
    /* Delegates to the canonical off/shadow/live parser (core/gate_mode.h);
     * this gate's unset default is OFF per feature-gate-requires-measurement. */
    switch (hu_gate_mode_parse(value, HU_GATE_OFF)) {
    case HU_GATE_LIVE:
        return HU_PROMPT_TRIM_LIVE;
    case HU_GATE_SHADOW:
        return HU_PROMPT_TRIM_SHADOW;
    default:
        return HU_PROMPT_TRIM_OFF;
    }
}

hu_prompt_trim_mode_t hu_prompt_trim_mode(void) {
    return hu_prompt_trim_mode_parse(getenv("HU_PROMPT_TRIM"));
}

size_t hu_prompt_trim_plan_floors(const char *buf, size_t len, size_t budget,
                                  const hu_prompt_trim_span_t *spans, size_t span_count,
                                  const size_t *floors, size_t *cuts_out) {
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
        /* A floor is the byte count of this span that must SURVIVE; the
         * cuttable region is what lies above it. floor >= avail means the
         * span is untouchable. */
        size_t floor_bytes = floors ? floors[i] : 0;
        if (avail <= floor_bytes)
            continue;
        size_t max_cut = avail - floor_bytes;
        size_t cut = needed < max_cut ? needed : max_cut;
        /* Extend a partial head cut forward to the next newline so the
         * surviving section content starts at a line boundary — but never
         * past the floor. A floor-capped cut may end mid-line. */
        while (cut < max_cut && buf[off + cut - 1] != '\n')
            cut++;
        cuts_out[i] = cut;
        total += cut;
        needed = needed > cut ? needed - cut : 0;
    }
    return total;
}

size_t hu_prompt_trim_plan(const char *buf, size_t len, size_t budget,
                           const hu_prompt_trim_span_t *spans, size_t span_count,
                           size_t *cuts_out) {
    return hu_prompt_trim_plan_floors(buf, len, budget, spans, span_count, NULL, cuts_out);
}

size_t hu_prompt_positional_cap_point(const char *buf, size_t len, size_t budget) {
    if (!buf || len <= budget)
        return len;
    /* Cut at the last newline within [0, budget) for a clean boundary;
     * accept the hard cap when no newline lies in the budget's upper
     * half (retreating further would delete too much). */
    size_t cut = budget;
    while (cut > 0 && buf[cut - 1] != '\n')
        cut--;
    if (cut < budget / 2)
        cut = budget;
    return cut;
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
