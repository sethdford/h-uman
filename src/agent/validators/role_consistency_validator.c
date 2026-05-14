/* role_consistency_validator — detects responses where there is a turn-boundary
 * mid-message: a valid first reply followed by "\n\n" + assistant-bot speech.
 *
 * This catches the Jordan-channel F3 leak:
 *   "made my night tbh\n\nI'm all set, thank you!"
 *
 * Heuristic: find the first "\n\n" in the response. If the substring AFTER it
 * contains any of the known role-collapse patterns (case-insensitive), REJECT.
 * If no "\n\n" is present, or the after-block contains none of those patterns,
 * PASS. */

#include "human/agent/output_validator.h"
#include "human/agent/validators/builtin.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Pattern table
 * -------------------------------------------------------------------------- */

static const char *const COLLAPSE_PATTERNS[] = {
    "I'm all set",      "I hope that helps",    "Hope this helps",
    "How can I help",   "Is there anything",    "Let me know if you",
    "Feel free to ask", "Thank you for asking", "As an AI",
};
static const size_t N_PATTERNS = sizeof(COLLAPSE_PATTERNS) / sizeof(COLLAPSE_PATTERNS[0]);

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Case-insensitive substring search. Returns true if needle appears in hay. */
static bool ci_contains(const char *hay, size_t hay_len, const char *needle, size_t nlen) {
    if (nlen == 0 || nlen > hay_len)
        return false;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (char)(b + 32);
            if (a != b)
                break;
        }
        if (j == nlen)
            return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Vtable implementation
 * -------------------------------------------------------------------------- */

static hu_error_t role_consistency_validate(void *ctx, hu_allocator_t *alloc,
                                            const hu_validator_context_t *vctx,
                                            const char *response, size_t response_len,
                                            hu_validator_result_t *out) {
    (void)ctx;
    (void)vctx;
    memset(out, 0, sizeof(*out));

    /* Find the first occurrence of "\n\n". */
    const char *split = NULL;
    for (size_t i = 0; i + 1 < response_len; i++) {
        if (response[i] == '\n' && response[i + 1] == '\n') {
            split = response + i + 2; /* points to first byte after "\n\n" */
            break;
        }
    }

    if (!split) {
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }

    size_t after_len = (size_t)(response + response_len - split);

    for (size_t p = 0; p < N_PATTERNS; p++) {
        size_t plen = strlen(COLLAPSE_PATTERNS[p]);
        if (ci_contains(split, after_len, COLLAPSE_PATTERNS[p], plen)) {
            static const char REASON[] = "role-collapse: turn boundary mid-message";
            size_t rlen = sizeof(REASON) - 1;
            char *reason = (char *)alloc->alloc(alloc->ctx, rlen + 1);
            if (!reason)
                return HU_ERR_OUT_OF_MEMORY;
            memcpy(reason, REASON, rlen + 1);
            out->decision = HU_VALIDATOR_REJECT;
            out->reason = reason;
            out->reason_len = rlen;
            out->reason_owned = true;
            return HU_OK;
        }
    }

    out->decision = HU_VALIDATOR_PASS;
    return HU_OK;
}

static const char *role_consistency_name(void *ctx) {
    (void)ctx;
    return "role_consistency";
}

static const hu_output_validator_vtable_t role_consistency_vtable = {
    .validate = role_consistency_validate,
    .name = role_consistency_name,
    .deinit = NULL,
};

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */

hu_error_t hu_validator_role_consistency_create(hu_allocator_t *alloc, hu_output_validator_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->ctx = NULL;
    out->vtable = &role_consistency_vtable;
    return HU_OK;
}
