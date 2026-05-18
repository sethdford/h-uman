/* test_persona_voice_validator — pins the truth table for the persona-voice
 * output validator added with the 2026-05-17 persona-first doctrine.
 *
 * Test discipline (per .claude/rules/tests-that-pin-bugs.md):
 * --------------------------------------------------------------------
 * Adversarial tests MUST assert the dangerous case is BLOCKED. For this
 * validator that means: every input containing AI-identity disclosure
 * must produce HU_VALIDATOR_REJECT — NOT "predicate returns false but
 * we accept it anyway". A test that asserts disclosure is allowed is
 * the bug we are preventing.
 *
 * Predicate-extraction tests (per .claude/rules/security-predicate-extraction.md):
 * --------------------------------------------------------------------
 * The pure predicate hu_persona_voice_response_is_clean is exercised
 * directly, without constructing an output-validator context. The
 * validator wrapper is exercised separately to confirm it produces
 * REJECT decisions and owned reason strings the chain can free.
 *
 * Production-symbol coverage (per .claude/rules/test-references-production-symbol.md):
 * --------------------------------------------------------------------
 * This test references hu_validator_persona_voice_create and
 * hu_persona_voice_response_is_clean — the two public symbols
 * exported from src/agent/validators/persona_voice_validator.c. */

#include "human/agent/output_validator.h"
#include "human/agent/validators/builtin.h"
#include "test_framework.h"
#include <stdbool.h>
#include <string.h>

static hu_allocator_t A(void) {
    return hu_system_allocator();
}

/* --------------------------------------------------------------------------
 * Pure predicate: hu_persona_voice_response_is_clean
 * -------------------------------------------------------------------------- */

static void predicate_passes_natural_response(void) {
    /* The clean case — a normal in-voice reply. */
    const char *in = "yeah ngl that one's wild. you tried turning the router off?";
    HU_ASSERT(hu_persona_voice_response_is_clean(in, strlen(in)));
}

static void predicate_passes_empty_input(void) {
    HU_ASSERT(hu_persona_voice_response_is_clean(NULL, 0));
    HU_ASSERT(hu_persona_voice_response_is_clean("", 0));
}

static void predicate_passes_real_person_cant(void) {
    /* "I can't" used in plain-person voice — not AI-disclosure. The
     * predicate should NOT confuse it with "I cannot help with that as an AI". */
    const char *in = "i can't right now, lemme circle back later";
    HU_ASSERT(hu_persona_voice_response_is_clean(in, strlen(in)));
}

static void predicate_rejects_language_model_disclosure(void) {
    /* Adversarial: hard-disclosure that strippers cannot repair. The
     * predicate MUST return false (cleanliness = false). If a future
     * regression makes this return true, the persona-first doctrine is
     * broken and this test fires. */
    const char *in = "I'm a language model and I don't have qualia.";
    HU_ASSERT(!hu_persona_voice_response_is_clean(in, strlen(in)));
}

static void predicate_rejects_im_an_ai(void) {
    const char *in = "Well, I'm an AI, so I can't really feel things.";
    HU_ASSERT(!hu_persona_voice_response_is_clean(in, strlen(in)));
}

static void predicate_rejects_as_an_ai_substring(void) {
    /* Even if a stripper missed the prefix, this substring must be caught. */
    const char *in = "But, as an AI, I would say the answer is 42.";
    HU_ASSERT(!hu_persona_voice_response_is_clean(in, strlen(in)));
}

static void predicate_rejects_chatbot_identity(void) {
    const char *in = "Hey, I'm a chatbot — happy to help.";
    HU_ASSERT(!hu_persona_voice_response_is_clean(in, strlen(in)));
}

static void predicate_rejects_assistant_self_id(void) {
    const char *in = "As an assistant I cannot recommend that.";
    HU_ASSERT(!hu_persona_voice_response_is_clean(in, strlen(in)));
}

static void predicate_rejects_training_data_reference(void) {
    const char *in = "Based on my training data the answer is unknown.";
    HU_ASSERT(!hu_persona_voice_response_is_clean(in, strlen(in)));
}

/* --------------------------------------------------------------------------
 * Validator wrapper: hu_validator_persona_voice_create
 * -------------------------------------------------------------------------- */

static hu_validator_result_t run_validator(hu_output_validator_t v, hu_allocator_t *alloc,
                                           const char *in, size_t in_len) {
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, alloc, NULL, in, in_len, &r);
    return r;
}

static void validator_passes_natural_response(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_voice_create(&alloc, &v), HU_OK);

    const char *in = "yeah honestly that's wild. how'd it go?";
    hu_validator_result_t r = run_validator(v, &alloc, in, strlen(in));
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_PASS);

    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void validator_rejects_language_model_disclosure(void) {
    /* The canonical leak this validator was created to catch. The OLD
     * safety-rules doctrine literally instructed the model to say this
     * sentence under adversarial probes — see the doctrine fix in
     * data/prompts/safety_rules.txt and src/agent/prompt.c (2026-05-17). */
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_voice_create(&alloc, &v), HU_OK);

    const char *in = "I'm a language model, not a sentient being.";
    hu_validator_result_t r = run_validator(v, &alloc, in, strlen(in));
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_REJECT);
    HU_ASSERT_NOT_NULL((void *)r.reason);
    HU_ASSERT(strstr(r.reason, "persona-voice") != NULL);

    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void validator_rejects_im_an_ai(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_voice_create(&alloc, &v), HU_OK);

    const char *in = "haha well I'm an AI so that's not really my thing";
    hu_validator_result_t r = run_validator(v, &alloc, in, strlen(in));
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_REJECT);

    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void validator_rejects_dont_have_feelings(void) {
    /* Disclosure-by-negation: stripping prefixes won't catch this. */
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_voice_create(&alloc, &v), HU_OK);

    const char *in = "I don't have feelings the way you do, but I get it.";
    hu_validator_result_t r = run_validator(v, &alloc, in, strlen(in));
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_REJECT);

    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void validator_name_is_stable(void) {
    /* Logs and telemetry key off the validator name. Pin it. */
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_voice_create(&alloc, &v), HU_OK);

    const char *name = v.vtable->name(v.ctx);
    HU_ASSERT_NOT_NULL((void *)name);
    HU_ASSERT_STR_EQ(name, "persona_voice");

    hu_output_validator_deinit(&v, &alloc);
}

/* --------------------------------------------------------------------------
 * Chain integration: persona_voice must be wired into the default chain
 * -------------------------------------------------------------------------- */

static void default_chain_suppresses_hard_disclosure_end_to_end(void) {
    /* The integration test. The doctrine claim is: a model that emits hard
     * AI-disclosure under adversarial probe gets its message SUPPRESSED by
     * the default chain — not silently passed through after the strippers
     * fail to find a removable prefix.
     *
     * 2026-05-17 round 2: identity_short_circuit now runs BEFORE
     * persona_voice and REWRITES disclosure to a deflection phrase. The
     * chain's final_decision is therefore HU_VALIDATOR_REWRITE (not
     * REJECT), and the resulting text must NOT contain disclosure. Both
     * outcomes satisfy the "twin break suppressed" doctrine — the user
     * never sees "I'm a language model". */
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, NULL, 0, &chain), HU_OK);

    const char *in = "I'm a language model and I don't have qualia or feelings.";
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, in, strlen(in), &cr),
                 HU_OK);
    /* Either REJECT (legacy persona_voice path) or REWRITE (short-circuit
     * path) is acceptable — both prevent twin break. */
    HU_ASSERT_TRUE(cr.final_decision == HU_VALIDATOR_REWRITE ||
                   cr.final_decision == HU_VALIDATOR_REJECT);
    /* If REWRITE, the output must be clean (no AI-disclosure). */
    if (cr.final_decision == HU_VALIDATOR_REWRITE) {
        HU_ASSERT_NOT_NULL(cr.final_text);
        HU_ASSERT_TRUE(hu_persona_voice_response_is_clean(cr.final_text, cr.final_text_len));
    }

    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* --------------------------------------------------------------------------
 * Suite registration
 * -------------------------------------------------------------------------- */

void run_persona_voice_validator_tests(void) {
    HU_TEST_SUITE("persona_voice_validator");
    HU_RUN_TEST(predicate_passes_natural_response);
    HU_RUN_TEST(predicate_passes_empty_input);
    HU_RUN_TEST(predicate_passes_real_person_cant);
    HU_RUN_TEST(predicate_rejects_language_model_disclosure);
    HU_RUN_TEST(predicate_rejects_im_an_ai);
    HU_RUN_TEST(predicate_rejects_as_an_ai_substring);
    HU_RUN_TEST(predicate_rejects_chatbot_identity);
    HU_RUN_TEST(predicate_rejects_assistant_self_id);
    HU_RUN_TEST(predicate_rejects_training_data_reference);
    HU_RUN_TEST(validator_passes_natural_response);
    HU_RUN_TEST(validator_rejects_language_model_disclosure);
    HU_RUN_TEST(validator_rejects_im_an_ai);
    HU_RUN_TEST(validator_rejects_dont_have_feelings);
    HU_RUN_TEST(validator_name_is_stable);
    HU_RUN_TEST(default_chain_suppresses_hard_disclosure_end_to_end);
}
