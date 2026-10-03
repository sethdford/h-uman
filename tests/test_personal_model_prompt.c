/* hu_turn_personal_model_prompt (src/agent/turn/personal_model_prompt.c): the
 * personal-model block agent_turn_run puts in the prompt. Carved out of
 * agent_turn.c in the 2026-10-02 train; these pin that the carve renders what
 * the inline block did (OFF) and keeps #608's confidence view (LIVE). */
#include "test_framework.h"

#include "human/agent.h"
#include "human/agent/turn.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/memory/confidence_boundary.h"
#include "human/memory/personal_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_pmp_a[] = "+15550000011"; /* told the twin something */
static const char k_pmp_b[] = "+15550000012"; /* is texting now */

static void pmp_add_fact(hu_personal_model_t *pm, const char *subj, const char *obj,
                         const char *handle, const char *channel) {
    hu_heuristic_fact_t *f = &pm->facts[pm->fact_count++];
    memset(f, 0, sizeof(*f));
    snprintf(f->subject, sizeof(f->subject), "%s", subj);
    snprintf(f->predicate, sizeof(f->predicate), "%s", "is");
    snprintf(f->object, sizeof(f->object), "%s", obj);
    f->confidence = 0.9f;
    f->last_seen_at = pm->updated_at;
    if (handle)
        snprintf(f->provenance.contact_handle, sizeof(f->provenance.contact_handle), "%s", handle);
    snprintf(f->provenance.channel, sizeof(f->provenance.channel), "%s", channel);
}

static hu_agent_t *pmp_agent(hu_allocator_t *a) {
    hu_agent_t *agent = (hu_agent_t *)calloc(1, sizeof(*agent));
    HU_ASSERT_NOT_NULL(agent);
    agent->alloc = a;
    hu_personal_model_init(&agent->personal_model);
    agent->personal_model.updated_at = 1767225600;
    agent->memory_session_id = k_pmp_b;
    agent->memory_session_id_len = sizeof(k_pmp_b) - 1;
    return agent;
}

static void personal_model_prompt_empty_model_renders_nothing(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_t *agent = pmp_agent(&a);
    static char buf[8192];
    HU_ASSERT_EQ(hu_turn_personal_model_prompt(agent, buf, sizeof(buf)), 0u);
    HU_ASSERT_EQ(hu_turn_personal_model_prompt(agent, NULL, sizeof(buf)), 0u);
    HU_ASSERT_EQ(hu_turn_personal_model_prompt(NULL, buf, sizeof(buf)), 0u);
    free(agent);
}

static void personal_model_prompt_off_is_the_plain_build(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_t *agent = pmp_agent(&a);
    pmp_add_fact(&agent->personal_model, "sister", "pregnant", k_pmp_a, "imessage_dm");
    pmp_add_fact(&agent->personal_model, "user", "an oat latte person", NULL, "cli");
    static char want[8192], got[8192];
    size_t want_n = hu_personal_model_build_prompt(&agent->personal_model, want, sizeof(want));
    HU_ASSERT_TRUE(want_n > 0);
    hu_confidence_set_mode_for_test(HU_GATE_OFF);
    size_t got_n = hu_turn_personal_model_prompt(agent, got, sizeof(got));
    hu_confidence_set_mode_for_test(-1);
    HU_ASSERT_EQ(got_n, want_n);
    HU_ASSERT_TRUE(memcmp(got, want, want_n) == 0);
    HU_ASSERT_NOT_NULL(strstr(got, "pregnant"));
    free(agent);
}

static void personal_model_prompt_live_keeps_another_contacts_fact_out(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_t *agent = pmp_agent(&a);
    pmp_add_fact(&agent->personal_model, "sister", "pregnant", k_pmp_a, "imessage_dm");
    pmp_add_fact(&agent->personal_model, "user", "an oat latte person", NULL, "cli");
    static char got[8192];
    hu_confidence_set_mode_for_test(HU_GATE_LIVE);
    size_t got_n = hu_turn_personal_model_prompt(agent, got, sizeof(got));
    hu_confidence_set_mode_for_test(-1);
    HU_ASSERT_TRUE(got_n > 0);
    HU_ASSERT_NULL(strstr(got, "pregnant"));
    HU_ASSERT_NOT_NULL(strstr(got, "oat latte"));
    HU_ASSERT_EQ(agent->personal_model.fact_count, 2); /* the agent's model is untouched */
    free(agent);
}

void run_personal_model_prompt_tests(void) {
    HU_TEST_SUITE("personal_model_prompt");
    HU_RUN_TEST(personal_model_prompt_empty_model_renders_nothing);
    HU_RUN_TEST(personal_model_prompt_off_is_the_plain_build);
    HU_RUN_TEST(personal_model_prompt_live_keeps_another_contacts_fact_out);
}
