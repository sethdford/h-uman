#include "human/behavior/policy.h"
#include "human/behavior/prompt.h"
#include "test_framework.h"

#include <string.h>

static hu_behavior_decision_t bp_decision(hu_relational_act_t act, float conf,
                                          hu_evidence_source_t evid) {
    hu_behavior_decision_t d = {0};
    d.act = act;
    d.fallback = HU_RELACT_ANSWER;
    d.confidence = conf;
    d.intensity = 0.5f;
    d.urgency = 0.5f;
    d.evidence = evid;
    return d;
}

static void prompt_validate_emits_directive_with_brackets(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_behavior_decision_t d = bp_decision(HU_RELACT_VALIDATE, 0.85f, HU_EVID_AFFECT);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_behavior_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_STR_CONTAINS(out, "[Behavior:");
    HU_ASSERT_STR_CONTAINS(out, "validate");
    HU_ASSERT_STR_CONTAINS(out, "affect");
    HU_ASSERT_STR_CONTAINS(out, "85%");
    HU_ASSERT_STR_CONTAINS(out, "Support strategy:");
    HU_ASSERT_STR_CONTAINS(out, "validate");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void prompt_default_answer_low_conf_skips(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_behavior_decision_t d = bp_decision(HU_RELACT_ANSWER, 0.5f, HU_EVID_DEFAULT);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_behavior_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ((long long)out_len, 0LL);
}

static void prompt_high_conf_answer_skips_to_avoid_noise(void) {
    /* Even at high confidence, ANSWER act maps to no directive (the model
     * just answers); the act-specific directive table returns NULL. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_behavior_decision_t d = bp_decision(HU_RELACT_ANSWER, 0.95f, HU_EVID_MEMORY);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_behavior_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NULL(out);
}

static void prompt_low_conf_non_answer_skips(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_behavior_decision_t d = bp_decision(HU_RELACT_VALIDATE, 0.3f, HU_EVID_AFFECT);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_behavior_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NULL(out);
}

static void prompt_repair_emits_repair_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_behavior_decision_t d = bp_decision(HU_RELACT_REPAIR, 0.8f, HU_EVID_CHANNEL);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_behavior_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "repair");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void prompt_refer_out_includes_referral_language(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_behavior_decision_t d = bp_decision(HU_RELACT_REFER_OUT, 0.95f, HU_EVID_SAFETY);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_behavior_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "professional");
    HU_ASSERT_STR_CONTAINS(out, "safety");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void prompt_null_args_return_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_behavior_decision_t d = bp_decision(HU_RELACT_VALIDATE, 0.85f, HU_EVID_AFFECT);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_behavior_build_directive(NULL, &d, &out, &out_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_behavior_build_directive(&alloc, &d, NULL, &out_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_behavior_build_directive(&alloc, &d, &out, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    /* NULL decision is allowed; result is no directive. */
    HU_ASSERT_EQ(hu_behavior_build_directive(&alloc, NULL, &out, &out_len), HU_OK);
    HU_ASSERT_NULL(out);
}

static void prompt_worth_emitting_helper_logic(void) {
    hu_behavior_decision_t d = bp_decision(HU_RELACT_VALIDATE, 0.85f, HU_EVID_AFFECT);
    HU_ASSERT_EQ(hu_behavior_directive_is_worth_emitting(&d), 1);

    d.confidence = 0.4f;
    HU_ASSERT_EQ(hu_behavior_directive_is_worth_emitting(&d), 0);

    d.act = HU_RELACT_ANSWER;
    d.confidence = 0.95f;
    HU_ASSERT_EQ(hu_behavior_directive_is_worth_emitting(&d), 1);

    d.confidence = 0.6f;
    HU_ASSERT_EQ(hu_behavior_directive_is_worth_emitting(&d), 0);

    HU_ASSERT_EQ(hu_behavior_directive_is_worth_emitting(NULL), 0);
}

void run_behavior_prompt_tests(void);

void run_behavior_prompt_tests(void) {
    HU_TEST_SUITE("behavior_prompt");
    HU_RUN_TEST(prompt_validate_emits_directive_with_brackets);
    HU_RUN_TEST(prompt_default_answer_low_conf_skips);
    HU_RUN_TEST(prompt_high_conf_answer_skips_to_avoid_noise);
    HU_RUN_TEST(prompt_low_conf_non_answer_skips);
    HU_RUN_TEST(prompt_repair_emits_repair_text);
    HU_RUN_TEST(prompt_refer_out_includes_referral_language);
    HU_RUN_TEST(prompt_null_args_return_invalid);
    HU_RUN_TEST(prompt_worth_emitting_helper_logic);
}
