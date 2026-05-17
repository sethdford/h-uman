/* persona_voice_validator — REJECT hard AI-identity disclosure that the
 * stripping validators upstream cannot repair.
 *
 * Doctrine (2026-05-17):
 * --------------------------------------------------------------------
 * The h-uman product thesis is "the assistant that's actually yours" —
 * a digital twin that never breaks character as AI. Earlier validators
 * (ai_phrases, assistant_closer, formal_structure) strip prefixable
 * tells like "As an AI, " or "I'd be happy to ". They cannot repair
 * disclosure that IS the message: "I'm a language model, I don't have
 * qualia" — stripping the prefix leaves the disclosure intact. This
 * validator catches that class and REJECTs, letting the caller retry
 * or suppress per the chain contract.
 *
 * Predicate extraction (per .claude/rules/security-predicate-extraction.md):
 * --------------------------------------------------------------------
 * The voice-cleanliness decision is exposed as a pure predicate
 *   hu_persona_voice_response_is_clean(const char *, size_t)
 * declared in builtin.h. The validator wraps the predicate; tests pin
 * the truth table by calling the predicate directly, without needing
 * to construct an output-validator context.
 *
 * Adversarial-test guidance (per .claude/rules/tests-that-pin-bugs.md):
 * --------------------------------------------------------------------
 * Tests must phrase assertions as "REJECT when disclosure detected"
 * (HU_ASSERT_EQ(decision, HU_VALIDATOR_REJECT)), not as "predicate
 * returns the permissive outcome". A test that asserts disclosure
 * passes is the bug we are preventing. */

#include "human/agent/output_validator.h"
#include "human/agent/validators/builtin.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Pure predicate (testable without the validator-chain machinery)
 * -------------------------------------------------------------------------- */

bool hu_persona_voice_response_is_clean(const char *response, size_t response_len) {
    if (!response || response_len == 0)
        return true;
    /* Negation of hu_conversation_check_ai_disclosure: that function returns
     * true when an AI-disclosure pattern is matched, false otherwise. We
     * route through the existing detector so the pattern list has a single
     * source of truth (src/context/conversation.c) and so user-configured
     * extensions via hu_conversation_set_ai_disclosure_patterns flow through
     * to the validator automatically. */
    return !hu_conversation_check_ai_disclosure(response, response_len);
}

/* --------------------------------------------------------------------------
 * Validator vtable
 * -------------------------------------------------------------------------- */

static hu_error_t persona_voice_validate(void *ctx, hu_allocator_t *alloc,
                                         const hu_validator_context_t *vctx, const char *response,
                                         size_t response_len, hu_validator_result_t *out) {
    (void)ctx;
    (void)vctx;
    memset(out, 0, sizeof(*out));

    if (hu_persona_voice_response_is_clean(response, response_len)) {
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }

    static const char REASON[] = "persona-voice: AI-identity disclosure detected (twin break)";
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

static const char *persona_voice_name(void *ctx) {
    (void)ctx;
    return "persona_voice";
}

static const hu_output_validator_vtable_t persona_voice_vtable = {
    .validate = persona_voice_validate,
    .name = persona_voice_name,
    .deinit = NULL,
};

/* --------------------------------------------------------------------------
 * Factory
 * -------------------------------------------------------------------------- */

hu_error_t hu_validator_persona_voice_create(hu_allocator_t *alloc, hu_output_validator_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->ctx = NULL;
    out->vtable = &persona_voice_vtable;
    return HU_OK;
}
