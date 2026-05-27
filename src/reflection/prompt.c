/* src/reflection/prompt.c — Reflection prompt + input transcript
 * assembly (T4).
 *
 * Spec: docs/plans/2026-05-26-reflection-loop/{design.md, tasks.md}
 * Task 4.
 *
 * Two responsibilities:
 *
 *   1. Hold the immutable system prompt as a static string literal.
 *      Matches the h-uman house convention (see
 *      src/agent/init_proposer.c:242::s_system_prompt). The trade-off
 *      with file-based templates is documented in the T4 STATUS
 *      heading: simpler build + no runtime failure mode + prompt
 *      lives next to the code that uses it, at the cost of needing
 *      a recompile to A/B prompts. The reflection prompt is
 *      expected to be quite stable in Phase 1; if tuning churn
 *      becomes a problem, T11's quorum work is when we'd move to
 *      an installable template.
 *
 *   2. Build the user-message body of the reflection LLM call from a
 *      pluggable turn-iterator. The iterator pattern decouples the
 *      data source (production: daemon turn ledger; tests: synthetic
 *      array) from the formatting logic. This module never imports
 *      daemon headers.
 *
 * Truncation strategy when max_chars is exceeded: drop OLDEST turns.
 * Rationale from spec: the most recent context is the most signal-
 * rich for pattern detection; the reflection model's job is to find
 * what's CHANGING, so the freshest tail is what it needs. The
 * alternative (drop newest) would degrade reflection quality fastest
 * exactly when conversation volume is highest — the worst case. */

#include "human/reflection.h"

#include "human/core/error.h"
#include "human/core/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── System prompt (immutable template) ──────────────────────────── */

static const char *const s_system_prompt =
    "You are a reflection layer over a personal-assistant transcript. Your job is "
    "to identify durable patterns in how the user (and their relationships) operate, "
    "based ONLY on the conversations provided. Do not invent observations not "
    "grounded in the transcript.\n"
    "\n"
    "Output STRICT JSON matching this schema (no prose, no markdown fences, no "
    "code blocks — the first character of your output MUST be `{` and the last "
    "must be `}`):\n"
    "{\n"
    "  \"patterns\": [\n"
    "    {\n"
    "      \"type\": \"topic_recurrence\" | \"behavioral_shift\" | \"preference\" "
    "| \"emotional_state\" | \"schedule_pattern\" | \"relationship\",\n"
    "      \"subject\": \"<string, up to 128 chars — usually 'Seth' or another named person>\",\n"
    "      \"observation\": \"<string, up to 512 chars, specific and grounded in evidence>\",\n"
    "      \"confidence\": <number in [0, 1]>,\n"
    "      \"evidence_ids\": [\"<turn_id_1>\", ...],\n"
    "      \"channels\": [\"<channel_name>\", ...]\n"
    "    }\n"
    "  ],\n"
    "  \"prose_summary\": \"<2-3 sentence summary of the most important patterns this run>\"\n"
    "}\n"
    "\n"
    "Rules:\n"
    "- Confidence < 0.5 means 'I noticed something but I'm unsure' — emit it; the "
    "storage layer will filter.\n"
    "- A pattern observed on a single occasion has lower confidence than one "
    "observed multiple times.\n"
    "- Prefer specificity ('Seth shifts to one-word replies after 9pm weeknights') "
    "over generality ('Seth is sometimes terse').\n"
    "- Do NOT emit patterns that restate stable known facts (e.g., \"Seth's name "
    "is Seth\"). Patterns are about CHANGES, RECURRENCES, or NEWLY-OBSERVABLE traits.\n"
    "- evidence_ids must reference the [id=...] tags from the user message.\n"
    "- channels must use the [channel=...] tag values verbatim.\n"
    "- Emit at most 15 patterns per run; choose the most signal-rich.";

const char *hu_reflection_system_prompt(void) {
    return s_system_prompt;
}

/* ── Helpers ────────────────────────────────────────────────────── */

/* Format ms-since-epoch as ISO-8601 UTC into out (must be >= 21 bytes
 * for "YYYY-MM-DDTHH:MM:SSZ" + NUL). Uses gmtime_r for thread safety.
 * The reflection LLM gets human-readable timestamps so it can reason
 * about temporal patterns ("evenings", "before meetings") without
 * doing epoch math itself. */
static void format_iso8601(uint64_t ts_ms, char *out, size_t cap) {
    if (cap < 21) {
        if (cap > 0)
            out[0] = '\0';
        return;
    }
    time_t secs = (time_t)(ts_ms / 1000);
    struct tm tm_utc;
    gmtime_r(&secs, &tm_utc);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

/* Append `s` to a heap buffer, growing if needed. *cap is the
 * allocation size, *len is the current strlen-ish length (excluding
 * NUL). On OOM, frees *buf and returns -1. */
static int buf_append(char **buf, size_t *cap, size_t *len, const char *s) {
    size_t add = strlen(s);
    if (*len + add + 1 > *cap) {
        size_t new_cap = *cap ? *cap * 2 : 256;
        while (new_cap < *len + add + 1)
            new_cap *= 2;
        char *p = (char *)realloc(*buf, new_cap);
        if (!p) {
            free(*buf);
            *buf = NULL;
            *cap = 0;
            *len = 0;
            return -1;
        }
        *buf = p;
        *cap = new_cap;
    }
    memcpy(*buf + *len, s, add);
    *len += add;
    (*buf)[*len] = '\0';
    return 0;
}

/* ── hu_reflection_build_input ───────────────────────────────────── */

/* We accumulate per-turn line offsets in a small dynamic array so the
 * "drop oldest turns until ≤ max_chars" pass can do O(1) shifts: we
 * find the first kept turn's offset and memmove the tail down. */
typedef struct {
    size_t offset; /* start byte of this turn's line in the buffer */
    size_t length; /* bytes including trailing '\n' */
} turn_extent_t;

hu_error_t hu_reflection_build_input(hu_reflection_turn_iter_fn iter_fn, void *iter_ctx,
                                     size_t max_chars, char **out_buf, int *out_turn_count) {
    if (!iter_fn || !out_buf || !out_turn_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_buf = NULL;
    *out_turn_count = 0;

    char *body = NULL;
    size_t cap = 0, len = 0;

    /* Dynamic array of per-turn extents for the truncation pass. */
    turn_extent_t *extents = NULL;
    size_t extents_cap = 0, extents_count = 0;

    /* Always begin with an empty NUL-terminated buffer so the zero-turn
     * case returns "" rather than NULL. */
    if (buf_append(&body, &cap, &len, "") != 0)
        return HU_ERR_OUT_OF_MEMORY;

    for (;;) {
        hu_reflection_turn_t turn = {0};
        if (!iter_fn(iter_ctx, &turn))
            break;

        char iso[32];
        format_iso8601(turn.ts_ms, iso, sizeof(iso));

        /* Build the line. We use snprintf to a small stack buffer for
         * the header + format, then directly append the content (which
         * may be unbounded — we'll truncate ON LINE granularity in the
         * drop-oldest pass below, not by clipping content mid-byte). */
        char header[256];
        snprintf(header, sizeof(header),
                 "[id=%s] [channel=%s] [ts=%s] %s: ", turn.turn_id ? turn.turn_id : "",
                 turn.channel ? turn.channel : "", iso, turn.sender ? turn.sender : "");

        size_t line_start = len;
        if (buf_append(&body, &cap, &len, header) != 0)
            goto oom;
        if (buf_append(&body, &cap, &len, turn.content ? turn.content : "") != 0)
            goto oom;
        if (buf_append(&body, &cap, &len, "\n") != 0)
            goto oom;

        if (extents_count == extents_cap) {
            size_t new_cap = extents_cap ? extents_cap * 2 : 16;
            turn_extent_t *p = (turn_extent_t *)realloc(extents, new_cap * sizeof(*p));
            if (!p)
                goto oom;
            extents = p;
            extents_cap = new_cap;
        }
        extents[extents_count].offset = line_start;
        extents[extents_count].length = len - line_start;
        extents_count++;
    }

    /* Truncation pass: drop oldest turns until len ≤ max_chars.
     * max_chars == 0 means "no cap" — skip. */
    size_t dropped = 0;
    if (max_chars > 0 && len > max_chars) {
        while (dropped < extents_count && len > max_chars) {
            size_t drop_len = extents[dropped].length;
            len -= drop_len;
            dropped++;
        }
        if (dropped > 0) {
            /* Shift the remaining bytes (everything past extents[dropped].offset)
             * down to offset 0. The OLD extents[dropped].offset has been
             * decremented from len, so the bytes we want to keep start at
             * the original extents[dropped].offset in the un-shifted buffer. */
            size_t keep_start = (dropped < extents_count) ? extents[dropped].offset : len;
            if (dropped < extents_count) {
                /* Tail bytes from keep_start onward become the new prefix. */
                memmove(body, body + keep_start, len);
            }
            body[len] = '\0';

            hu_log_info("reflection.prompt", NULL,
                        "input transcript truncated: dropped %zu oldest turns to fit %zu-char cap",
                        dropped, max_chars);
        }
    }

    free(extents);
    *out_buf = body;
    *out_turn_count = (int)(extents_count - dropped);
    return HU_OK;

oom:
    free(body);
    free(extents);
    return HU_ERR_OUT_OF_MEMORY;
}
