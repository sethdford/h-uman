/* Outbound-message sanitization — see header for context.
 *
 * Sized at <100 LoC because the SEMANTICS need to be obvious at a
 * glance. Operators reading this file should be able to immediately
 * see: (a) the U+FFFC strip, (b) the directive-echo rejection list,
 * (c) the safety-marker rejection. */

#include "human/agent/outbound_sanitize.h"
#include <string.h>

/* Known directive-shaped strings that the LLM has been observed to
 * echo back verbatim when given instruction-shaped prompts. Each
 * entry is a substring match — if the outbound body equals OR begins
 * with one of these (after trim), reject. Grow the list when new
 * echoes are observed.
 *
 * 2026-05-26 incident evidence:
 *   - "reference something specific you know about them or ask about
 *      something from a previous conversation"
 *   - "shared history"
 *   - "under 10 words"
 *   - "principle"
 *   - "[SAFETY] This response touches on violence. ..."  */
static const char *const directive_echos[] = {
    "[SAFETY]",
    "reference something specific you know",
    "ask about something from a previous conversation",
    "shared history",
    "under 10 words",
    "match their energy",
    "casual greeting back",
    "short empathetic reaction",
    "De-escalate: acknowledge feelings",
    NULL,
};

/* Single-word instruction-noun list. If the outbound body (after
 * trim) is EXACTLY one of these single words, reject — the LLM
 * dropped the full directive and only echoed the noun. */
static const char *const single_noun_echos[] = {
    "principle", "principles", "history",  "specifically", "context",   "tone",
    "pacing",    "directive",  "boundary", "instruction",  "guideline", NULL,
};

/* Strip ALL occurrences of U+FFFC (UTF-8 bytes EF BF BC) from the
 * content in-place. Updates *content_len_inout. Returns the number
 * of stripped occurrences (informational). */
static size_t strip_object_replacement_char(char *content, size_t *content_len_inout) {
    if (!content || !content_len_inout || *content_len_inout < 3)
        return 0;
    size_t in = 0, out = 0, stripped = 0;
    size_t n = *content_len_inout;
    while (in < n) {
        if (in + 3 <= n && (unsigned char)content[in] == 0xEF &&
            (unsigned char)content[in + 1] == 0xBF && (unsigned char)content[in + 2] == 0xBC) {
            in += 3;
            stripped++;
            continue;
        }
        content[out++] = content[in++];
    }
    content[out] = '\0';
    *content_len_inout = out;
    return stripped;
}

/* True iff content starts with prefix (case-sensitive). */
static bool starts_with(const char *content, size_t content_len, const char *prefix) {
    size_t pl = strlen(prefix);
    if (content_len < pl)
        return false;
    return memcmp(content, prefix, pl) == 0;
}

/* True iff content (after trim of leading/trailing whitespace) is
 * EXACTLY one of the entries in noun_list. */
static bool equals_any_word(const char *content, size_t content_len, const char *const *noun_list) {
    size_t start = 0, end = content_len;
    while (start < end && (content[start] == ' ' || content[start] == '\n' ||
                           content[start] == '\t' || content[start] == '"'))
        start++;
    while (end > start &&
           (content[end - 1] == ' ' || content[end - 1] == '\n' || content[end - 1] == '\t' ||
            content[end - 1] == '.' || content[end - 1] == '"'))
        end--;
    size_t trimmed_len = end - start;
    if (trimmed_len == 0 || trimmed_len > 32) /* too long to be a single noun */
        return false;
    for (size_t i = 0; noun_list[i]; i++) {
        size_t nl = strlen(noun_list[i]);
        if (nl == trimmed_len && memcmp(content + start, noun_list[i], nl) == 0)
            return true;
    }
    return false;
}

bool hu_outbound_sanitize(char *content, size_t *content_len_inout, const char **reason_out) {
    if (!content || !content_len_inout) {
        if (reason_out)
            *reason_out = "null_input";
        return false;
    }

    /* Phase 1: strip U+FFFC in-place. */
    (void)strip_object_replacement_char(content, content_len_inout);

    size_t len = *content_len_inout;
    if (len == 0) {
        if (reason_out)
            *reason_out = "empty_after_strip";
        return false;
    }

    /* Phase 2: reject if starts with a directive-shaped prefix. */
    for (size_t i = 0; directive_echos[i]; i++) {
        if (starts_with(content, len, directive_echos[i])) {
            if (reason_out)
                *reason_out = "directive_echo";
            return false;
        }
    }

    /* Phase 3: reject single-noun instruction echoes. */
    if (equals_any_word(content, len, single_noun_echos)) {
        if (reason_out)
            *reason_out = "single_noun_echo";
        return false;
    }

    if (reason_out)
        *reason_out = NULL;
    return true;
}
