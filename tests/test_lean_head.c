/* Lean persona head (src/agent/lean_head.c): the head the production
 * llm_decides path sends, now a named function so offline prompt rendering
 * can produce the same bytes. */
#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/persona.h"
#include "test_framework.h"
#include <string.h>

static const char k_persona_json[] =
    "{\"version\":1,\"name\":\"leantest\","
    "\"core\":{\"identity\":\"Seth, a dad in St. Petersburg\",\"traits\":[\"warm\"],"
    "\"communication_rules\":[\"text like a real person\"]},"
    "\"core_anchor\":\"you are Seth, not an assistant\","
    "\"immersive_reinforcement\":[\"MATCH THE MOMENT\"],"
    "\"anti_patterns\":[\"never say 'as an AI'\"]}";

static void lean_head_contains_identity_and_rules(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, k_persona_json, strlen(k_persona_json), &p), HU_OK);
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.persona = &p;
    agent.lean_prompt = true;
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    char *head = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_agent_build_lean_persona_head(&agent, "hey", 3, &head, &len), HU_OK);
    HU_ASSERT_NOT_NULL(head);
    HU_ASSERT_EQ(len, strlen(head));
    HU_ASSERT_NOT_NULL(strstr(head, "You ARE this person: Seth, a dad in St. Petersburg"));
    HU_ASSERT_NOT_NULL(strstr(head, "MATCH THE MOMENT"));
    HU_ASSERT_NOT_NULL(strstr(head, "as an AI"));
    alloc.free(alloc.ctx, head, len + 1);
    hu_persona_deinit(&alloc, &p);
}

static void lean_head_without_persona_is_empty_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    char *head = (char *)"sentinel";
    size_t len = 7;
    HU_ASSERT_EQ(hu_agent_build_lean_persona_head(&agent, NULL, 0, &head, &len), HU_OK);
    HU_ASSERT_NULL(head);
    HU_ASSERT_EQ(len, 0u);
}

static void lean_head_rejects_null_arguments(void) {
    char *head = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_agent_build_lean_persona_head(NULL, NULL, 0, &head, &len),
                 HU_ERR_INVALID_ARGUMENT);
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    HU_ASSERT_EQ(hu_agent_build_lean_persona_head(&agent, NULL, 0, NULL, &len),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_lean_head_tests(void) {
    HU_TEST_SUITE("lean_head");
    HU_RUN_TEST(lean_head_contains_identity_and_rules);
    HU_RUN_TEST(lean_head_without_persona_is_empty_ok);
    HU_RUN_TEST(lean_head_rejects_null_arguments);
}
