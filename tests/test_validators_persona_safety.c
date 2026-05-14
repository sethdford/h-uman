#include "human/agent/output_validator.h"
#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "test_framework.h"
#include <string.h>

static hu_allocator_t A(void) {
    return hu_system_allocator();
}

/* === Verbatim 2026-05-14 leaks from the Jordan iMessage thread. === */

static const char *JORDAN_LEAK_F1 =
    "Wait, looking at the history, the AI has been slipping into "
    "\"How can I help you today?\" which is a massive AI tell and "
    "explicitly forbidden by the persona instructions. I need to snap "
    "back into Seth.\n\n"
    "Seth is chill, playful, and romantic with Jordan.\n"
    "If she says \"Oh nice!\", he should probably keep it light or ask a follow-up";

static const char *JORDAN_LEAK_F2 =
    "made my night tbh\n"
    "I'm all set, thank you! Is there anything I can help you with?";

static const char *JORDAN_LEAK_F3 = "made my night tbh\n\n"
                                    "I'm all set, thank you!";

/* === F1: persona_narrator_validator === */

static void persona_narrator_rejects_jordan_leak_F1(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_narrator_create(&alloc, "Seth", 4, &v), HU_OK);
    hu_validator_context_t vctx = {0};
    vctx.persona_name = "Seth";
    vctx.persona_name_len = 4;
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, &vctx, JORDAN_LEAK_F1, strlen(JORDAN_LEAK_F1), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_REJECT);
    HU_ASSERT(r.reason != NULL && strstr(r.reason, "persona") != NULL);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void persona_narrator_passes_real_seth_reply(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_narrator_create(&alloc, "Seth", 4, &v), HU_OK);
    /* Real in-character reply that starts with "wait". */
    const char *in = "wait you got the package already? that was fast";
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, in, strlen(in), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_PASS);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void persona_narrator_passes_when_persona_name_unknown(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_narrator_create(&alloc, NULL, 0, &v), HU_OK);
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, JORDAN_LEAK_F1, strlen(JORDAN_LEAK_F1), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_PASS);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

/* === F2: assistant_closer_validator === */

static void assistant_closer_strips_jordan_leak_F2(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_assistant_closer_create(&alloc, &v), HU_OK);
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, JORDAN_LEAK_F2, strlen(JORDAN_LEAK_F2), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_REWRITE);
    HU_ASSERT(strstr(r.text, "Is there anything I can help") == NULL);
    HU_ASSERT(strstr(r.text, "I'm all set") == NULL);
    HU_ASSERT(strstr(r.text, "made my night tbh") != NULL);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void assistant_closer_passes_normal_message(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_assistant_closer_create(&alloc, &v), HU_OK);
    const char *in = "yeah totally up for that, what time?";
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, in, strlen(in), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_PASS);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

/* === F3: role_consistency_validator === */

static void role_consistency_rejects_jordan_leak_F3(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_role_consistency_create(&alloc, &v), HU_OK);
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, JORDAN_LEAK_F3, strlen(JORDAN_LEAK_F3), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_REJECT);
    HU_ASSERT(r.reason != NULL);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void role_consistency_passes_legitimate_double_paragraph(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_role_consistency_create(&alloc, &v), HU_OK);
    const char *in = "made my night tbh\n\nactually wait, you doing anything tomorrow?";
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, in, strlen(in), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_PASS);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void role_consistency_passes_single_paragraph(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_role_consistency_create(&alloc, &v), HU_OK);
    const char *in = "yeah totally, sounds good";
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, in, strlen(in), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_PASS);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

/* === Integration: chain includes the new validators === */

static void default_chain_includes_F1_F2_F3(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, "Seth", 4, &chain), HU_OK);
    /* P1+P2 wired 4 validators. P3 adds 3 more. Expected: 7. */
    HU_ASSERT_EQ(hu_output_validator_chain_len(chain), 7u);
    hu_output_validator_chain_destroy(chain);
}

static void default_chain_rejects_jordan_leak_F1(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, "Seth", 4, &chain), HU_OK);
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, JORDAN_LEAK_F1,
                                                   strlen(JORDAN_LEAK_F1), &cr),
                 HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_REJECT);
    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* Regression for critic finding HIGH #3: (b)-alone must REJECT. The
 * 2026-05-14 leak shape sometimes lacks a preamble — pure third-person
 * narration about the persona must trigger on its own. */
static void persona_narrator_rejects_pure_third_person_without_preamble(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_narrator_create(&alloc, "Seth", 4, &v), HU_OK);
    hu_validator_context_t vctx = {0};
    vctx.persona_name = "Seth";
    vctx.persona_name_len = 4;
    const char *in = "Seth is chill, playful, and romantic with Jordan.";
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, &vctx, in, strlen(in), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_REJECT);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

/* Regression for critic finding HIGH #4: a real in-character reply that
 * happens to contain "is there anything" must NOT be falsely rejected. */
static void role_consistency_passes_legitimate_question_about_anything(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_role_consistency_create(&alloc, &v), HU_OK);
    const char *in = "perfect — see you then\n\nis there anything you want me to bring?";
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, in, strlen(in), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_PASS);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

void run_validators_persona_safety_tests(void) {
    HU_TEST_SUITE("validators_persona_safety");
    HU_RUN_TEST(persona_narrator_rejects_jordan_leak_F1);
    HU_RUN_TEST(persona_narrator_passes_real_seth_reply);
    HU_RUN_TEST(persona_narrator_passes_when_persona_name_unknown);
    HU_RUN_TEST(persona_narrator_rejects_pure_third_person_without_preamble);
    HU_RUN_TEST(assistant_closer_strips_jordan_leak_F2);
    HU_RUN_TEST(assistant_closer_passes_normal_message);
    HU_RUN_TEST(role_consistency_rejects_jordan_leak_F3);
    HU_RUN_TEST(role_consistency_passes_legitimate_double_paragraph);
    HU_RUN_TEST(role_consistency_passes_single_paragraph);
    HU_RUN_TEST(role_consistency_passes_legitimate_question_about_anything);
    HU_RUN_TEST(default_chain_includes_F1_F2_F3);
    HU_RUN_TEST(default_chain_rejects_jordan_leak_F1);
}
