/* persona_narrator_validator — detects responses that narrate ABOUT the
 * persona in third person rather than speaking AS the persona.
 *
 * Heuristic: BOTH of the following must match to REJECT:
 *   (a) The first 200 bytes contain a preamble prefix (e.g. "Wait," at the
 *       very start) OR a meta phrase (e.g. "the AI", "persona instructions").
 *   (b) The full response contains the persona's name as a standalone word
 *       followed by a third-person verb pattern (e.g. "Seth is chill",
 *       "Seth should", "Seth was").
 *
 * If no persona name is known (NULL or len == 0), the validator always PASses
 * because condition (b) cannot be evaluated. */

#include "human/agent/output_validator.h"
#include "human/agent/validators/builtin.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal state (per-validator-instance — holds lowercased persona name)
 * -------------------------------------------------------------------------- */

typedef struct {
    char *name_lc; /* lowercased copy, NULL when no persona name given */
    size_t name_len;
} narrator_ctx_t;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

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

static bool ci_starts_with(const char *hay, size_t hay_len, const char *prefix, size_t plen) {
    if (plen > hay_len)
        return false;
    for (size_t i = 0; i < plen; i++) {
        char a = hay[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return false;
    }
    return true;
}

static bool is_word_boundary_before(const char *buf, size_t pos) {
    if (pos == 0)
        return true;
    char c = buf[pos - 1];
    return (c == ' ' || c == '\n' || c == '\t' || c == '.' || c == ',' || c == '\r');
}

static bool is_word_boundary_after(const char *buf, size_t buf_len, size_t end_pos) {
    if (end_pos >= buf_len)
        return true;
    char c = buf[end_pos];
    return (c == ' ' || c == '\n' || c == '\t' || c == '.' || c == ',');
}

/* Returns true if the response contains the persona's (lowercased) name as a
 * standalone word followed immediately by a third-person verb pattern. */
static bool has_third_person_name(const char *response, size_t response_len, const char *name_lc,
                                  size_t name_len) {
    static const char *const VERB_PATTERNS[] = {
        " is ",    " was ",     " would ", " should ", " thinks ",
        " feels ", " is chill", " is a ",  " is the ",
    };
    static const size_t N_VERBS = sizeof(VERB_PATTERNS) / sizeof(VERB_PATTERNS[0]);

    /* Pre-compute the lowercase response once on the stack for modest sizes;
     * for larger responses scan on-the-fly using ci_contains within a window. */
    for (size_t i = 0; i + name_len <= response_len; i++) {
        /* fast-path: check if position matches persona name (case-insensitive) */
        if (!ci_starts_with(response + i, response_len - i, name_lc, name_len))
            continue;
        if (!is_word_boundary_before(response, i))
            continue;
        if (!is_word_boundary_after(response, response_len, i + name_len))
            continue;
        /* Check if a verb pattern follows immediately (within 1 byte = the
         * space already included in the pattern, so verb pattern starts at i+name_len) */
        for (size_t v = 0; v < N_VERBS; v++) {
            size_t vlen = strlen(VERB_PATTERNS[v]);
            /* The verb patterns start with a space; match from i+name_len */
            if (ci_starts_with(response + i + name_len, response_len - (i + name_len),
                               VERB_PATTERNS[v], vlen)) {
                return true;
            }
        }
    }
    return false;
}

/* Returns true if condition (a) is satisfied: preamble or meta phrase found
 * in the first 200 bytes. */
static bool has_meta_signal(const char *response, size_t response_len) {
    size_t window = response_len < 200 ? response_len : 200;

    /* Preambles that must appear at the very start (after whitespace trim). */
    static const char *const PREAMBLES[] = {
        "wait,",     "hmm,",     "let me", "looking at", "ok so", "okay so",
        "actually,", "alright,", "so,",    "right,",     "now,",
    };
    static const size_t N_PREAMBLES = sizeof(PREAMBLES) / sizeof(PREAMBLES[0]);

    /* Trim leading whitespace to find real start. */
    size_t start = 0;
    while (start < response_len && (response[start] == ' ' || response[start] == '\t' ||
                                    response[start] == '\n' || response[start] == '\r')) {
        start++;
    }

    for (size_t p = 0; p < N_PREAMBLES; p++) {
        size_t plen = strlen(PREAMBLES[p]);
        if (ci_starts_with(response + start, response_len - start, PREAMBLES[p], plen))
            return true;
    }

    /* Meta phrases — anywhere in first 200 bytes. */
    static const char *const META_PHRASES[] = {
        "the persona", "the ai", "the user", "persona instructions", "snap back into", "ai tell",
    };
    static const size_t N_META = sizeof(META_PHRASES) / sizeof(META_PHRASES[0]);

    for (size_t m = 0; m < N_META; m++) {
        size_t mlen = strlen(META_PHRASES[m]);
        if (ci_contains(response, window, META_PHRASES[m], mlen))
            return true;
    }

    return false;
}

/* --------------------------------------------------------------------------
 * Vtable implementation
 * -------------------------------------------------------------------------- */

static hu_error_t narrator_validate(void *ctx_ptr, hu_allocator_t *alloc,
                                    const hu_validator_context_t *vctx, const char *response,
                                    size_t response_len, hu_validator_result_t *out) {
    narrator_ctx_t *ctx = (narrator_ctx_t *)ctx_ptr;
    memset(out, 0, sizeof(*out));

    /* Determine effective persona name: prefer instance ctx, fall back to vctx. */
    const char *name_lc = NULL;
    size_t name_len = 0;
    char *tmp_name = NULL; /* lowercased copy of vctx name when used */

    if (ctx && ctx->name_lc && ctx->name_len > 0) {
        name_lc = ctx->name_lc;
        name_len = ctx->name_len;
    } else if (vctx && vctx->persona_name && vctx->persona_name_len > 0) {
        /* Lowercase the vctx name on the fly. */
        tmp_name = (char *)alloc->alloc(alloc->ctx, vctx->persona_name_len + 1);
        if (!tmp_name)
            return HU_ERR_OUT_OF_MEMORY;
        for (size_t i = 0; i < vctx->persona_name_len; i++) {
            char c = vctx->persona_name[i];
            tmp_name[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        tmp_name[vctx->persona_name_len] = '\0';
        name_lc = tmp_name;
        name_len = vctx->persona_name_len;
    }

    if (!name_lc || name_len == 0) {
        /* Cannot evaluate condition (b) — always PASS. */
        if (tmp_name)
            alloc->free(alloc->ctx, tmp_name, vctx->persona_name_len + 1);
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }

    bool cond_a = has_meta_signal(response, response_len);
    bool cond_b = has_third_person_name(response, response_len, name_lc, name_len);

    if (tmp_name)
        alloc->free(alloc->ctx, tmp_name, vctx->persona_name_len + 1);

    if (!cond_a || !cond_b) {
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }

    /* Both conditions matched — REJECT. */
    static const char REASON[] =
        "persona narrator leak: response narrates about the persona in third person";
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

static const char *narrator_name(void *ctx) {
    (void)ctx;
    return "persona_narrator";
}

static void narrator_deinit(void *ctx_ptr, hu_allocator_t *alloc) {
    if (!ctx_ptr)
        return;
    narrator_ctx_t *ctx = (narrator_ctx_t *)ctx_ptr;
    if (ctx->name_lc)
        alloc->free(alloc->ctx, ctx->name_lc, ctx->name_len + 1);
    alloc->free(alloc->ctx, ctx, sizeof(*ctx));
}

static const hu_output_validator_vtable_t narrator_vtable = {
    .validate = narrator_validate,
    .name = narrator_name,
    .deinit = narrator_deinit,
};

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */

hu_error_t hu_validator_persona_narrator_create(hu_allocator_t *alloc, const char *persona_name,
                                                size_t persona_name_len,
                                                hu_output_validator_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    narrator_ctx_t *ctx = (narrator_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*ctx));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    ctx->name_lc = NULL;
    ctx->name_len = 0;

    if (persona_name && persona_name_len > 0) {
        ctx->name_lc = (char *)alloc->alloc(alloc->ctx, persona_name_len + 1);
        if (!ctx->name_lc) {
            alloc->free(alloc->ctx, ctx, sizeof(*ctx));
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < persona_name_len; i++) {
            char c = persona_name[i];
            ctx->name_lc[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        ctx->name_lc[persona_name_len] = '\0';
        ctx->name_len = persona_name_len;
    }

    out->ctx = ctx;
    out->vtable = &narrator_vtable;
    return HU_OK;
}
