#include "human/behavior/trust.h"
#include "human/behavior/trust_prompt.h"
#include "test_framework.h"

#include <string.h>

static hu_trust_decision_t tp_decision(hu_trust_action_t a, float firmness, const char *rationale) {
    hu_trust_decision_t d = {0};
    d.action = a;
    d.firmness = firmness;
    d.confidence = 0.85f;
    if (rationale) {
        snprintf(d.rationale, sizeof(d.rationale), "%s", rationale);
    }
    return d;
}

static void tp_answer_emits_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_decision_t d = tp_decision(HU_TRUST_ANSWER, 0.0f, "default");
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ((long long)out_len, 0LL);
}

static void tp_push_back_emits_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_decision_t d = tp_decision(HU_TRUST_PUSH_BACK, 0.7f, "memory disagrees");
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "[Trust:");
    HU_ASSERT_STR_CONTAINS(out, "push_back");
    HU_ASSERT_STR_CONTAINS(out, "Disagree");
    HU_ASSERT_STR_CONTAINS(out, "70%");
    HU_ASSERT_STR_CONTAINS(out, "memory disagrees");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void tp_refuse_to_agree_includes_anti_capitulation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_decision_t d = tp_decision(HU_TRUST_REFUSE_TO_AGREE, 0.95f, "tool disagrees under pressure");
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "refuse_to_agree");
    HU_ASSERT_STR_CONTAINS(out, "do not capitulate");
    HU_ASSERT_STR_CONTAINS(out, "95%");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void tp_disclose_uncertainty_includes_no_flatter(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_decision_t d = tp_decision(HU_TRUST_DISCLOSE_UNCERTAINTY, 0.4f, "user invoked authority");
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "disclose_uncertainty");
    HU_ASSERT_STR_CONTAINS(out, "Do not flatter");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void tp_cite_memory_includes_citation_request(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_decision_t d = tp_decision(HU_TRUST_CITE_MEMORY, 0.0f, "high trust");
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "Cite the memory");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void tp_abstain_includes_decline_language(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_decision_t d = tp_decision(HU_TRUST_ABSTAIN, 0.0f, "no info");
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "Decline");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void tp_refer_out_includes_professional(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_decision_t d = tp_decision(HU_TRUST_REFER_OUT, 0.0f, "scope");
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, &d, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "professional");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void tp_null_args_return_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_decision_t d = tp_decision(HU_TRUST_PUSH_BACK, 0.5f, "");
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_trust_build_directive(NULL, &d, &out, &out_len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, &d, NULL, &out_len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, &d, &out, NULL), HU_ERR_INVALID_ARGUMENT);
    /* NULL decision is allowed; result is no directive. */
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, NULL, &out, &out_len), HU_OK);
    HU_ASSERT_NULL(out);
}

static void tp_worth_emitting_helper(void) {
    hu_trust_decision_t d = tp_decision(HU_TRUST_PUSH_BACK, 0.5f, "x");
    HU_ASSERT_EQ(hu_trust_directive_is_worth_emitting(&d), 1);
    d.action = HU_TRUST_ANSWER;
    HU_ASSERT_EQ(hu_trust_directive_is_worth_emitting(&d), 0);
    HU_ASSERT_EQ(hu_trust_directive_is_worth_emitting(NULL), 0);
}

void run_behavior_trust_prompt_tests(void);

void run_behavior_trust_prompt_tests(void) {
    HU_TEST_SUITE("behavior_trust_prompt");
    HU_RUN_TEST(tp_answer_emits_nothing);
    HU_RUN_TEST(tp_push_back_emits_directive);
    HU_RUN_TEST(tp_refuse_to_agree_includes_anti_capitulation);
    HU_RUN_TEST(tp_disclose_uncertainty_includes_no_flatter);
    HU_RUN_TEST(tp_cite_memory_includes_citation_request);
    HU_RUN_TEST(tp_abstain_includes_decline_language);
    HU_RUN_TEST(tp_refer_out_includes_professional);
    HU_RUN_TEST(tp_null_args_return_invalid);
    HU_RUN_TEST(tp_worth_emitting_helper);
}
