/* assistant_closer_validator — strips known AI-helper closing phrases from
 * responses. Always REWRITE if any phrase is found; PASS otherwise.
 *
 * This catches the Jordan-channel F2 leak where assistant-bot speech appended
 * to an in-character reply:
 *   "made my night tbh\nI'm all set, thank you! Is there anything I can help
 *    you with?"
 *
 * After stripping all matches, trailing whitespace and newlines are trimmed.
 * If nothing was stripped the validator returns PASS. */

#include "human/agent/output_validator.h"
#include "human/agent/validators/builtin.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Phrase table (case-insensitive strip-anywhere)
 * -------------------------------------------------------------------------- */

static const char *const CLOSER_PHRASES[] = {
    "How can I help you today?",
    "Is there anything I can help you with?",
    "Is there anything else I can help you with?",
    "Let me know if you have any other questions",
    "Feel free to ask if you have more questions",
    "I'm all set, thank you!",
    "I hope that helps!",
    "Hope this helps!",
};
static const size_t N_CLOSERS = sizeof(CLOSER_PHRASES) / sizeof(CLOSER_PHRASES[0]);

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Case-insensitive character equality. */
static bool ci_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z')
        a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z')
        b = (char)(b + 32);
    return a == b;
}

/* Find the first case-insensitive occurrence of `needle` (length nlen) in
 * `hay[0..hay_len)`. Returns the byte offset, or SIZE_MAX if not found. */
static size_t ci_find(const char *hay, size_t hay_len, const char *needle, size_t nlen) {
    if (nlen == 0 || nlen > hay_len)
        return SIZE_MAX;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (!ci_eq(hay[i + j], needle[j])) {
                match = false;
                break;
            }
        }
        if (match)
            return i;
    }
    return SIZE_MAX;
}

/* Remove all case-insensitive occurrences of `needle` from `buf` (which has
 * capacity >= buf_len + 1 and is NUL-terminated). Updates *len_inout.
 * Returns true if at least one occurrence was removed. */
static bool strip_all_occurrences(char *buf, size_t *len_inout, const char *needle, size_t nlen) {
    bool stripped = false;
    size_t len = *len_inout;
    size_t search_from = 0;

    while (search_from < len) {
        size_t pos = ci_find(buf + search_from, len - search_from, needle, nlen);
        if (pos == SIZE_MAX)
            break;
        size_t abs_pos = search_from + pos;
        /* Shift everything after the phrase left. */
        size_t tail = len - (abs_pos + nlen);
        memmove(buf + abs_pos, buf + abs_pos + nlen, tail);
        len -= nlen;
        buf[len] = '\0';
        stripped = true;
        /* Don't advance search_from — there could be another occurrence at the
         * same absolute position after the shift. */
    }

    *len_inout = len;
    return stripped;
}

/* Trim trailing whitespace (space, tab, newline, carriage-return) in-place. */
static void trim_trailing_whitespace(char *buf, size_t *len_inout) {
    size_t len = *len_inout;
    while (len > 0) {
        char c = buf[len - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            len--;
        } else {
            break;
        }
    }
    buf[len] = '\0';
    *len_inout = len;
}

/* --------------------------------------------------------------------------
 * Vtable implementation
 * -------------------------------------------------------------------------- */

static hu_error_t closer_validate(void *ctx, hu_allocator_t *alloc,
                                  const hu_validator_context_t *vctx, const char *response,
                                  size_t response_len, hu_validator_result_t *out) {
    (void)ctx;
    (void)vctx;
    memset(out, 0, sizeof(*out));

    /* Allocate a working buffer (response_len + 1 for NUL). */
    char *buf = (char *)alloc->alloc(alloc->ctx, response_len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, response, response_len);
    buf[response_len] = '\0';

    size_t len = response_len;
    bool any_stripped = false;

    for (size_t p = 0; p < N_CLOSERS; p++) {
        size_t plen = strlen(CLOSER_PHRASES[p]);
        if (strip_all_occurrences(buf, &len, CLOSER_PHRASES[p], plen))
            any_stripped = true;
    }

    if (any_stripped)
        trim_trailing_whitespace(buf, &len);

    if (!any_stripped || (len == response_len && memcmp(buf, response, len) == 0)) {
        alloc->free(alloc->ctx, buf, response_len + 1);
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }

    out->decision = HU_VALIDATOR_REWRITE;
    out->text = buf;
    out->text_len = len;
    out->text_owned = true;
    return HU_OK;
}

static const char *closer_name(void *ctx) {
    (void)ctx;
    return "assistant_closer";
}

static const hu_output_validator_vtable_t closer_vtable = {
    .validate = closer_validate,
    .name = closer_name,
    .deinit = NULL,
};

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */

hu_error_t hu_validator_assistant_closer_create(hu_allocator_t *alloc, hu_output_validator_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->ctx = NULL;
    out->vtable = &closer_vtable;
    return HU_OK;
}
