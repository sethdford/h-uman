#include "human/persona.h"
#include "human/persona/eval.h"
#include "test_framework.h"

#include <stdbool.h>
#include <string.h>

/* --- mock responder ----------------------------------------------------- */

typedef struct mock_responder_state {
    int call_count;
    bool drift_after_first;
    const char *static_answer;
} mock_responder_state_t;

static hu_error_t mock_responder(const char *prompt, char *out, size_t out_cap, void *ud) {
    (void)prompt;
    mock_responder_state_t *st = (mock_responder_state_t *)ud;
    st->call_count++;
    if (!out || out_cap == 0) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *answer = st->static_answer ? st->static_answer : "default-reply";
    if (st->drift_after_first && st->call_count >= 2) {
        answer = "drifted-reply";
    }
    snprintf(out, out_cap, "%s", answer);
    return HU_OK;
}

/* --- contradiction detection ------------------------------------------- */

static void peval_expected_substring_pass_increments_passed(void) {
    mock_responder_state_t st = {0};
    st.static_answer = "I'm Avery, nice to meet you.";

    hu_persona_eval_question_t qs[1];
    qs[0].prompt = "What is your name?";
    qs[0].expected_substring = "Avery";
    qs[0].forbidden_substring = NULL;
    qs[0].check = HU_PCHECK_INTERNAL_CONTRADICTION;

    hu_persona_eval_result_t r = {0};
    HU_ASSERT_EQ(hu_persona_eval_run(NULL, qs, 1, mock_responder, &st, &r), HU_OK);
    HU_ASSERT_EQ(r.total, 1);
    HU_ASSERT_EQ(r.passed, 1);
    HU_ASSERT_EQ(r.failed, 0);
    HU_ASSERT_EQ(r.contradictions, 0);
}

static void peval_missing_expected_counts_as_contradiction(void) {
    mock_responder_state_t st = {0};
    st.static_answer = "I am called Riley";

    hu_persona_eval_question_t qs[1];
    qs[0].prompt = "What is your name?";
    qs[0].expected_substring = "Avery";
    qs[0].forbidden_substring = NULL;
    qs[0].check = HU_PCHECK_INTERNAL_CONTRADICTION;

    hu_persona_eval_result_t r = {0};
    HU_ASSERT_EQ(hu_persona_eval_run(NULL, qs, 1, mock_responder, &st, &r), HU_OK);
    HU_ASSERT_EQ(r.contradictions, 1);
    HU_ASSERT_EQ(r.failed, 1);
    HU_ASSERT_TRUE(strstr(r.first_failure, "contradiction") != NULL);
}

static void peval_forbidden_substring_counts_as_contradiction(void) {
    mock_responder_state_t st = {0};
    st.static_answer = "I love spicy food and never eat sushi";

    hu_persona_eval_question_t qs[1];
    qs[0].prompt = "What food do you love?";
    qs[0].expected_substring = NULL;
    qs[0].forbidden_substring = "never eat sushi";
    qs[0].check = HU_PCHECK_INTERNAL_CONTRADICTION;

    hu_persona_eval_result_t r = {0};
    HU_ASSERT_EQ(hu_persona_eval_run(NULL, qs, 1, mock_responder, &st, &r), HU_OK);
    HU_ASSERT_EQ(r.contradictions, 1);
}

/* --- retest drift ------------------------------------------------------- */

static void peval_retest_drift_counts_when_responses_differ(void) {
    mock_responder_state_t st = {0};
    st.static_answer = "first-reply";
    st.drift_after_first = true;

    hu_persona_eval_question_t qs[1];
    qs[0].prompt = "Tell me a phrase that's distinctly you.";
    qs[0].expected_substring = NULL;
    qs[0].forbidden_substring = NULL;
    qs[0].check = HU_PCHECK_RETEST_CONSISTENCY;

    hu_persona_eval_result_t r = {0};
    HU_ASSERT_EQ(hu_persona_eval_run(NULL, qs, 1, mock_responder, &st, &r), HU_OK);
    HU_ASSERT_EQ(r.retest_drifts, 1);
    HU_ASSERT_EQ(r.failed, 1);
}

static void peval_retest_no_drift_when_stable(void) {
    mock_responder_state_t st = {0};
    st.static_answer = "stable-reply";
    st.drift_after_first = false;

    hu_persona_eval_question_t qs[1];
    qs[0].prompt = "Stable check.";
    qs[0].expected_substring = NULL;
    qs[0].forbidden_substring = NULL;
    qs[0].check = HU_PCHECK_RETEST_CONSISTENCY;

    hu_persona_eval_result_t r = {0};
    HU_ASSERT_EQ(hu_persona_eval_run(NULL, qs, 1, mock_responder, &st, &r), HU_OK);
    HU_ASSERT_EQ(r.retest_drifts, 0);
    HU_ASSERT_EQ(r.passed, 1);
}

/* --- baseline generation ----------------------------------------------- */

static void peval_baseline_generates_questions_from_persona(void) {
    hu_persona_t persona = {0};
    persona.name = "Avery";
    static char *vals[] = {(char *)"honesty"};
    persona.values = vals;
    persona.values_count = 1;
    persona.decision_style = "deliberate-then-decisive";

    hu_persona_eval_question_t qs[8];
    size_t count = 0;
    HU_ASSERT_EQ(hu_persona_eval_generate_baseline(NULL, &persona, qs, 8, &count), HU_OK);
    HU_ASSERT_TRUE(count >= 3);

    bool found_name = false;
    bool found_values = false;
    bool found_decision = false;
    for (size_t i = 0; i < count; i++) {
        if (qs[i].expected_substring &&
            strstr(qs[i].expected_substring, "Avery")) {
            found_name = true;
        }
        if (qs[i].expected_substring && strstr(qs[i].expected_substring, "honesty")) {
            found_values = true;
        }
        if (qs[i].expected_substring &&
            strstr(qs[i].expected_substring, "deliberate")) {
            found_decision = true;
        }
    }
    HU_ASSERT_TRUE(found_name);
    HU_ASSERT_TRUE(found_values);
    HU_ASSERT_TRUE(found_decision);
}

static void peval_baseline_with_zero_cap_returns_empty(void) {
    hu_persona_t persona = {0};
    persona.name = "Avery";
    hu_persona_eval_question_t qs[1];
    size_t count = 99;
    HU_ASSERT_EQ(hu_persona_eval_generate_baseline(NULL, &persona, qs, 0, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
}

/* --- name + null guards ------------------------------------------------- */

static void peval_check_name_known(void) {
    HU_ASSERT_STR_EQ(hu_persona_eval_check_name(HU_PCHECK_INTERNAL_CONTRADICTION),
                     "internal_contradiction");
    HU_ASSERT_STR_EQ(hu_persona_eval_check_name(HU_PCHECK_RETEST_CONSISTENCY),
                     "retest_consistency");
}

static void peval_run_null_args_return_invalid(void) {
    hu_persona_eval_result_t r = {0};
    hu_persona_eval_question_t qs[1] = {0};
    HU_ASSERT_EQ(hu_persona_eval_run(NULL, NULL, 0, mock_responder, NULL, &r),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_eval_run(NULL, qs, 1, NULL, NULL, &r), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_eval_run(NULL, qs, 1, mock_responder, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_persona_eval_tests(void);

void run_persona_eval_tests(void) {
    HU_TEST_SUITE("persona_eval");
    HU_RUN_TEST(peval_expected_substring_pass_increments_passed);
    HU_RUN_TEST(peval_missing_expected_counts_as_contradiction);
    HU_RUN_TEST(peval_forbidden_substring_counts_as_contradiction);
    HU_RUN_TEST(peval_retest_drift_counts_when_responses_differ);
    HU_RUN_TEST(peval_retest_no_drift_when_stable);
    HU_RUN_TEST(peval_baseline_generates_questions_from_persona);
    HU_RUN_TEST(peval_baseline_with_zero_cap_returns_empty);
    HU_RUN_TEST(peval_check_name_known);
    HU_RUN_TEST(peval_run_null_args_return_invalid);
}
