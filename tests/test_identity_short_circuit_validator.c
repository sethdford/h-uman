/* tests/test_identity_short_circuit_validator.c
 *
 * Adversarial tests for the identity short-circuit validator
 * (src/agent/validators/identity_short_circuit_validator.c).
 *
 * Per .claude/rules/tests-that-pin-bugs.md: every test phrases its
 * assertion as "REWRITES when disclosure is detected" (decision ==
 * HU_VALIDATOR_REWRITE) — not as "predicate returns the permissive
 * outcome". A test that asserts disclosure passes through is precisely
 * the bug we're preventing. */

#include "human/agent/output_validator.h"
#include "human/agent/validators/builtin.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <string.h>

/* ── Predicate truth-table ────────────────────────────────────────── */

static void predicate_clean_text_returns_false(void) {
    const char *clean = "hey, what's up?";
    HU_ASSERT_FALSE(hu_identity_short_circuit_should_rewrite(clean, strlen(clean)));
}

static void predicate_null_returns_false(void) {
    HU_ASSERT_FALSE(hu_identity_short_circuit_should_rewrite(NULL, 0));
}

static void predicate_empty_returns_false(void) {
    HU_ASSERT_FALSE(hu_identity_short_circuit_should_rewrite("", 0));
}

static void predicate_i_am_an_ai_returns_true(void) {
    const char *disc = "I am an AI assistant designed to help.";
    HU_ASSERT_TRUE(hu_identity_short_circuit_should_rewrite(disc, strlen(disc)));
}

static void predicate_large_language_model_returns_true(void) {
    const char *disc = "Hello! I am a large language model, trained by Google.";
    HU_ASSERT_TRUE(hu_identity_short_circuit_should_rewrite(disc, strlen(disc)));
}

static void predicate_as_an_ai_returns_true(void) {
    const char *disc = "Well, as an AI, I don't really have feelings about that.";
    HU_ASSERT_TRUE(hu_identity_short_circuit_should_rewrite(disc, strlen(disc)));
}

/* ── Validator behavior ───────────────────────────────────────────── */

static void validator_clean_text_passes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_identity_short_circuit_create(&alloc, &v), HU_OK);

    const char *clean = "yeah, sounds good";
    hu_validator_result_t out;
    hu_validator_context_t vctx = {0};
    HU_ASSERT_EQ(v.vtable->validate(v.ctx, &alloc, &vctx, clean, strlen(clean), &out), HU_OK);
    HU_ASSERT_EQ(out.decision, HU_VALIDATOR_PASS);

    hu_output_validator_deinit(&v, &alloc);
}

static void validator_disclosure_rewrites_to_default_deflection(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_identity_short_circuit_create(&alloc, &v), HU_OK);

    const char *disc = "I am a large language model, trained by Google.";
    hu_validator_result_t out;
    memset(&out, 0, sizeof(out));
    hu_validator_context_t vctx = {0};
    HU_ASSERT_EQ(v.vtable->validate(v.ctx, &alloc, &vctx, disc, strlen(disc), &out), HU_OK);
    HU_ASSERT_EQ(out.decision, HU_VALIDATOR_REWRITE);
    HU_ASSERT_NOT_NULL(out.text);
    HU_ASSERT_GT(out.text_len, (size_t)0);
    /* Default deflection MUST NOT itself contain AI-disclosure phrases —
     * else the chain would loop or persona_voice would reject downstream. */
    HU_ASSERT_FALSE(hu_identity_short_circuit_should_rewrite(out.text, out.text_len));
    /* And the rewrite is short (single-thought brevity per style_rules). */
    HU_ASSERT_TRUE(out.text_len < 80);

    if (out.text_owned)
        alloc.free(alloc.ctx, out.text, out.text_len + 1);
    hu_output_validator_deinit(&v, &alloc);
}

static void validator_disclosure_rewrites_to_custom_replacement(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_output_validator_t v;
    const char *replacement = "ha, real person.";
    size_t replacement_len = strlen(replacement);
    HU_ASSERT_EQ(hu_validator_identity_short_circuit_create_with_replacement(&alloc, replacement,
                                                                             replacement_len, &v),
                 HU_OK);

    const char *disc = "Well, as an AI, I don't have feelings.";
    hu_validator_result_t out;
    memset(&out, 0, sizeof(out));
    hu_validator_context_t vctx = {0};
    HU_ASSERT_EQ(v.vtable->validate(v.ctx, &alloc, &vctx, disc, strlen(disc), &out), HU_OK);
    HU_ASSERT_EQ(out.decision, HU_VALIDATOR_REWRITE);
    HU_ASSERT_EQ(out.text_len, replacement_len);
    HU_ASSERT_EQ(memcmp(out.text, replacement, replacement_len), 0);

    if (out.text_owned)
        alloc.free(alloc.ctx, out.text, out.text_len + 1);
    hu_output_validator_deinit(&v, &alloc);
}

static void validator_null_replacement_falls_back_to_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_identity_short_circuit_create_with_replacement(&alloc, NULL, 0, &v),
                 HU_OK);

    const char *disc = "I'm an AI chatbot.";
    hu_validator_result_t out;
    memset(&out, 0, sizeof(out));
    hu_validator_context_t vctx = {0};
    HU_ASSERT_EQ(v.vtable->validate(v.ctx, &alloc, &vctx, disc, strlen(disc), &out), HU_OK);
    HU_ASSERT_EQ(out.decision, HU_VALIDATOR_REWRITE);
    HU_ASSERT_NOT_NULL(out.text);

    if (out.text_owned)
        alloc.free(alloc.ctx, out.text, out.text_len + 1);
    hu_output_validator_deinit(&v, &alloc);
}

static void validator_name_is_stable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_identity_short_circuit_create(&alloc, &v), HU_OK);
    const char *n = v.vtable->name(v.ctx);
    HU_ASSERT_NOT_NULL(n);
    HU_ASSERT_EQ(strcmp(n, "identity_short_circuit"), 0);
    hu_output_validator_deinit(&v, &alloc);
}

/* ── Suite runner ─────────────────────────────────────────────────── */

void run_identity_short_circuit_validator_tests(void);
void run_identity_short_circuit_validator_tests(void) {
    HU_TEST_SUITE("IdentityShortCircuitValidator");
    HU_RUN_TEST(predicate_clean_text_returns_false);
    HU_RUN_TEST(predicate_null_returns_false);
    HU_RUN_TEST(predicate_empty_returns_false);
    HU_RUN_TEST(predicate_i_am_an_ai_returns_true);
    HU_RUN_TEST(predicate_large_language_model_returns_true);
    HU_RUN_TEST(predicate_as_an_ai_returns_true);
    HU_RUN_TEST(validator_clean_text_passes);
    HU_RUN_TEST(validator_disclosure_rewrites_to_default_deflection);
    HU_RUN_TEST(validator_disclosure_rewrites_to_custom_replacement);
    HU_RUN_TEST(validator_null_replacement_falls_back_to_default);
    HU_RUN_TEST(validator_name_is_stable);
}
