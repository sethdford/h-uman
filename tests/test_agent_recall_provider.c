/* hu_agent_internal_recall_provider (src/agent/agent.c): which provider the
 * contact-recall planner may call. The LLM planner costs ~5.5 s on every
 * message over 12 words (2026-10-01 trace); HU_RECALL_PLANNER_LLM lets it be
 * switched off for an A/B. */
#include "../src/agent/agent_internal.h"
#include "human/agent.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

static const hu_provider_vtable_t k_fake_vt = {0};
static const char k_long[] = "so the landlord wants me to sign another full year and i honestly "
                             "do not know if i want to stay here";

static void recall_provider_follows_register_and_gate(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.provider.vtable = &k_fake_vt;

    unsetenv("HU_RECALL_PLANNER_LLM"); /* default: today's behavior */
    HU_ASSERT_TRUE(hu_agent_internal_recall_provider(&agent, k_long, strlen(k_long)) ==
                   &agent.provider);
    HU_ASSERT_NULL(hu_agent_internal_recall_provider(&agent, "hey whats up", 12)); /* casual */

    setenv("HU_RECALL_PLANNER_LLM", "off", 1);
    HU_ASSERT_NULL(hu_agent_internal_recall_provider(&agent, k_long, strlen(k_long)));
    setenv("HU_RECALL_PLANNER_LLM", "shadow", 1); /* nothing to shadow: it routes, or not */
    HU_ASSERT_NULL(hu_agent_internal_recall_provider(&agent, k_long, strlen(k_long)));
    setenv("HU_RECALL_PLANNER_LLM", "live", 1);
    HU_ASSERT_TRUE(hu_agent_internal_recall_provider(&agent, k_long, strlen(k_long)) ==
                   &agent.provider);
    unsetenv("HU_RECALL_PLANNER_LLM");
}

void run_agent_recall_provider_tests(void) {
    HU_TEST_SUITE("agent recall provider");
    HU_RUN_TEST(recall_provider_follows_register_and_gate);
}
