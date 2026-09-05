/* llm_decides gate split — safety gates stay on, heuristic gates follow the flag.
 *
 * Incident 2026-09-01: production runs channels.imessage.daemon.llm_decides=true,
 * which disabled the consecutive-response limiter, the AI-tell retry, and the
 * quality retry along with the heuristic classifier gates. "I apologize for the
 * delay in responding" reached a real contact with nothing in the way. */
#include "human/daemon/reactive_gates.h"
#include "test_framework.h"
#include <string.h>

static const hu_reactive_gate_t k_heuristic[] = {
    HU_REACTIVE_GATE_RESPONSE_MODE, HU_REACTIVE_GATE_DROP_OFF,       HU_REACTIVE_GATE_TAPBACK_SKIP,
    HU_REACTIVE_GATE_LEAVE_ON_READ, HU_REACTIVE_GATE_CONSTITUTIONAL,
};
static const hu_reactive_gate_t k_safety[] = {
    HU_REACTIVE_GATE_CONSECUTIVE_LIMIT,
    HU_REACTIVE_GATE_AI_TELL_RETRY,
    HU_REACTIVE_GATE_QUALITY_RETRY,
};

static void test_heuristic_gates_off_when_llm_decides(void) {
    for (size_t i = 0; i < sizeof(k_heuristic) / sizeof(k_heuristic[0]); i++) {
        HU_ASSERT_FALSE(hu_reactive_gate_is_safety(k_heuristic[i]));
        HU_ASSERT_FALSE(hu_reactive_gate_active(k_heuristic[i], true));
    }
}

static void test_heuristic_gates_on_when_not_llm_decides(void) {
    for (size_t i = 0; i < sizeof(k_heuristic) / sizeof(k_heuristic[0]); i++)
        HU_ASSERT_TRUE(hu_reactive_gate_active(k_heuristic[i], false));
}

static void test_safety_gates_on_regardless_of_llm_decides(void) {
    for (size_t i = 0; i < sizeof(k_safety) / sizeof(k_safety[0]); i++) {
        HU_ASSERT_TRUE(hu_reactive_gate_is_safety(k_safety[i]));
        HU_ASSERT_TRUE(hu_reactive_gate_active(k_safety[i], true));
        HU_ASSERT_TRUE(hu_reactive_gate_active(k_safety[i], false));
    }
}

static void test_unknown_gate_fails_closed(void) {
    HU_ASSERT_TRUE(hu_reactive_gate_active((hu_reactive_gate_t)999, true));
}

/* ── AI-tell detector ───────────────────────────────────────────────── */

static void test_ai_tell_clean_reply_is_null(void) {
    HU_ASSERT_NULL(hu_reactive_response_ai_tell("lol nah coconut water is weird"));
    HU_ASSERT_NULL(hu_reactive_response_ai_tell("yeah for sure"));
    HU_ASSERT_NULL(hu_reactive_response_ai_tell(""));
    HU_ASSERT_NULL(hu_reactive_response_ai_tell(NULL));
}

static void test_ai_tell_legacy_phrases_still_detected(void) {
    HU_ASSERT_NOT_NULL(hu_reactive_response_ai_tell("I'd be happy to help with that"));
    HU_ASSERT_NOT_NULL(hu_reactive_response_ai_tell("sorry to hear that man"));
    HU_ASSERT_NOT_NULL(hu_reactive_response_ai_tell("According to my records"));
}

static void test_ai_tell_incident_phrases_detected(void) {
    /* The four that reached real contacts on 2026-09-01. */
    const char *hit;
    hit = hu_reactive_response_ai_tell("I apologize for the delay in responding");
    HU_ASSERT_NOT_NULL(hit);
    hit = hu_reactive_response_ai_tell("I understand you're experiencing these feelings");
    HU_ASSERT_NOT_NULL(hit);
    hit = hu_reactive_response_ai_tell("could you please clarify what you'd like to know?");
    HU_ASSERT_NOT_NULL(hit);
    hit = hu_reactive_response_ai_tell("I understand you are frustrated");
    HU_ASSERT_NOT_NULL(hit);
}

static void test_ai_tell_is_case_insensitive_and_names_phrase(void) {
    const char *hit = hu_reactive_response_ai_tell("i APOLOGIZE FOR THE DELAY, got busy");
    HU_ASSERT_NOT_NULL(hit);
    HU_ASSERT_NOT_NULL(strstr(hit, "apologize for the delay"));
}

static void test_ai_tell_does_not_flag_human_sorry(void) {
    /* Seth's own register: "sorry just saw this" / "my bad" must stay clean,
     * and so must a plain human apology — "I apologize for the mistake!" is
     * real Seth text in data/eval_blinded_ab.json. */
    HU_ASSERT_NULL(hu_reactive_response_ai_tell("sorry just saw this"));
    HU_ASSERT_NULL(hu_reactive_response_ai_tell("ha my bad just saw this"));
    HU_ASSERT_NULL(hu_reactive_response_ai_tell("sorry, got slammed today"));
    HU_ASSERT_NULL(hu_reactive_response_ai_tell("I apologize for the mistake!"));
    HU_ASSERT_NULL(hu_reactive_response_ai_tell("I apologize, my bad"));
}

/* ── AI-tell action: send / retry once / drop on second miss ────────── */

static void test_ai_tell_action_clean_reply_sends(void) {
    HU_ASSERT_EQ((int)hu_reactive_ai_tell_action(NULL, false), (int)HU_AI_TELL_SEND);
    HU_ASSERT_EQ((int)hu_reactive_ai_tell_action(NULL, true), (int)HU_AI_TELL_SEND);
}

static void test_ai_tell_action_first_miss_retries(void) {
    HU_ASSERT_EQ((int)hu_reactive_ai_tell_action("I apologize for the delay", false),
                 (int)HU_AI_TELL_RETRY);
}

static void test_ai_tell_action_second_miss_drops(void) {
    /* The retry hint already fired and the model still produced a tell:
     * silence beats sending a therapy-bot line to a friend. */
    HU_ASSERT_EQ((int)hu_reactive_ai_tell_action("I apologize for the delay", true),
                 (int)HU_AI_TELL_DROP);
}

void run_reactive_gates_tests(void) {
    HU_TEST_SUITE("Reactive Gate Split");
    HU_RUN_TEST(test_ai_tell_action_clean_reply_sends);
    HU_RUN_TEST(test_ai_tell_action_first_miss_retries);
    HU_RUN_TEST(test_ai_tell_action_second_miss_drops);
    HU_RUN_TEST(test_heuristic_gates_off_when_llm_decides);
    HU_RUN_TEST(test_heuristic_gates_on_when_not_llm_decides);
    HU_RUN_TEST(test_safety_gates_on_regardless_of_llm_decides);
    HU_RUN_TEST(test_unknown_gate_fails_closed);
    HU_RUN_TEST(test_ai_tell_clean_reply_is_null);
    HU_RUN_TEST(test_ai_tell_legacy_phrases_still_detected);
    HU_RUN_TEST(test_ai_tell_incident_phrases_detected);
    HU_RUN_TEST(test_ai_tell_is_case_insensitive_and_names_phrase);
    HU_RUN_TEST(test_ai_tell_does_not_flag_human_sorry);
}
