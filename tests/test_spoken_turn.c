/* tests/test_spoken_turn.c — latency-first prompt profile for voice turns.
 *
 * Pins the caps, the gate, the per-turn begin/end that the gateway wraps around
 * hu_agent_turn_stream_v2, and the effect on the lean persona head: fewer
 * examples and a spoken-style directive. */
#include "human/agent.h"
#include "human/agent/spoken_turn.h"
#include "human/core/allocator.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static void spoken_turn_caps_shrink_memory_and_examples(void) {
    size_t entries = 0, chars = 0;
    hu_spoken_turn_memory_caps(true, &entries, &chars);
    HU_ASSERT_EQ(entries, (size_t)HU_SPOKEN_TURN_MEMORY_ENTRIES);
    HU_ASSERT_EQ(chars, (size_t)HU_SPOKEN_TURN_MEMORY_CHARS);
    hu_spoken_turn_memory_caps(false, &entries, &chars);
    HU_ASSERT_EQ(entries, (size_t)HU_TEXT_MEMORY_ENTRIES);
    HU_ASSERT_EQ(chars, (size_t)HU_TEXT_MEMORY_CHARS);
    HU_ASSERT_TRUE(HU_SPOKEN_TURN_MEMORY_ENTRIES < HU_TEXT_MEMORY_ENTRIES);
    HU_ASSERT_TRUE(HU_SPOKEN_TURN_MEMORY_CHARS < HU_TEXT_MEMORY_CHARS);
    HU_ASSERT_EQ(hu_spoken_turn_example_cap(true), (size_t)HU_SPOKEN_TURN_EXAMPLES);
    HU_ASSERT_EQ(hu_spoken_turn_example_cap(false), (size_t)HU_TEXT_EXAMPLES);
    hu_spoken_turn_memory_caps(true, NULL, NULL); /* NULL-safe */
}

static void spoken_turn_mode_reads_env_default_off(void) {
    const char *prev = getenv("HU_SPOKEN_TURN");
    char *saved = prev ? strdup(prev) : NULL;
    unsetenv("HU_SPOKEN_TURN");
    HU_ASSERT_EQ((int)hu_spoken_turn_mode(), (int)HU_GATE_OFF);
    setenv("HU_SPOKEN_TURN", "shadow", 1);
    HU_ASSERT_EQ((int)hu_spoken_turn_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_SPOKEN_TURN", "live", 1);
    HU_ASSERT_EQ((int)hu_spoken_turn_mode(), (int)HU_GATE_LIVE);
    if (saved) {
        setenv("HU_SPOKEN_TURN", saved, 1);
        free(saved);
    } else {
        unsetenv("HU_SPOKEN_TURN");
    }
}

static void spoken_turn_begin_live_sets_and_end_restores(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_spoken_turn_saved_t saved;
    hu_spoken_turn_begin(&agent, HU_GATE_LIVE, &saved);
    HU_ASSERT_TRUE(agent.lean_prompt);
    HU_ASSERT_TRUE(agent.spoken_turn);
    hu_spoken_turn_end(&agent, &saved);
    HU_ASSERT_FALSE(agent.lean_prompt);
    HU_ASSERT_FALSE(agent.spoken_turn);
}

static void spoken_turn_end_restores_a_lean_prompt_that_was_already_on(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.lean_prompt = true; /* e.g. set by the daemon's llm_decides path */
    hu_spoken_turn_saved_t saved;
    hu_spoken_turn_begin(&agent, HU_GATE_LIVE, &saved);
    hu_spoken_turn_end(&agent, &saved);
    HU_ASSERT_TRUE(agent.lean_prompt);
    HU_ASSERT_FALSE(agent.spoken_turn);
}

static void spoken_turn_begin_off_and_shadow_leave_agent_alone(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_spoken_turn_saved_t saved;
    hu_spoken_turn_begin(&agent, HU_GATE_OFF, &saved);
    HU_ASSERT_FALSE(agent.lean_prompt);
    HU_ASSERT_FALSE(agent.spoken_turn);
    hu_spoken_turn_begin(&agent, HU_GATE_SHADOW, &saved);
    HU_ASSERT_FALSE(agent.lean_prompt);
    HU_ASSERT_FALSE(agent.spoken_turn);
    hu_spoken_turn_begin(NULL, HU_GATE_LIVE, &saved); /* NULL-safe */
    hu_spoken_turn_end(NULL, &saved);
}

/* ── effect on the lean persona head ───────────────────────────────────────── */

static const char k_persona_json[] =
    "{\"version\":1,\"name\":\"spokentest\","
    "\"core\":{\"identity\":\"a dad in St. Petersburg\",\"traits\":[\"warm\"],"
    "\"communication_rules\":[\"text like a real person\"]},"
    "\"example_banks\":[{\"channel\":\"gateway\",\"examples\":["
    "{\"context\":\"c\",\"incoming\":\"in-one\",\"response\":\"out-one\"},"
    "{\"context\":\"c\",\"incoming\":\"in-two\",\"response\":\"out-two\"},"
    "{\"context\":\"c\",\"incoming\":\"in-three\",\"response\":\"out-three\"},"
    "{\"context\":\"c\",\"incoming\":\"in-four\",\"response\":\"out-four\"}]}]}";

static size_t count_occurrences(const char *hay, const char *needle) {
    size_t n = 0, nl = strlen(needle);
    for (const char *p = strstr(hay, needle); p; p = strstr(p + nl, needle))
        n++;
    return n;
}

static char *build_head(hu_allocator_t *alloc, hu_persona_t *p, bool spoken, size_t *len) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = alloc;
    agent.persona = p;
    agent.lean_prompt = true;
    agent.spoken_turn = spoken;
    agent.active_channel = "gateway";
    agent.active_channel_len = 7;
    char *head = NULL;
    HU_ASSERT_EQ(hu_agent_build_lean_persona_head(&agent, "hey", 3, &head, len), HU_OK);
    return head;
}

static void spoken_turn_lean_head_has_fewer_examples_and_spoken_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, k_persona_json, strlen(k_persona_json), &p), HU_OK);

    size_t text_len = 0, spoken_len = 0;
    char *text_head = build_head(&alloc, &p, false, &text_len);
    char *spoken_head = build_head(&alloc, &p, true, &spoken_len);
    HU_ASSERT_NOT_NULL(text_head);
    HU_ASSERT_NOT_NULL(spoken_head);

    HU_ASSERT_EQ(count_occurrences(text_head, "them: in-"), 4u);
    HU_ASSERT_EQ(count_occurrences(spoken_head, "them: in-"), (size_t)HU_SPOKEN_TURN_EXAMPLES);
    HU_ASSERT_NULL(strstr(text_head, "spoken voice conversation"));
    HU_ASSERT_NOT_NULL(strstr(spoken_head, "spoken voice conversation"));

    alloc.free(alloc.ctx, text_head, text_len + 1);
    alloc.free(alloc.ctx, spoken_head, spoken_len + 1);
    hu_persona_deinit(&alloc, &p);
}

void run_spoken_turn_tests(void) {
    HU_TEST_SUITE("spoken_turn");
    HU_RUN_TEST(spoken_turn_caps_shrink_memory_and_examples);
    HU_RUN_TEST(spoken_turn_mode_reads_env_default_off);
    HU_RUN_TEST(spoken_turn_begin_live_sets_and_end_restores);
    HU_RUN_TEST(spoken_turn_end_restores_a_lean_prompt_that_was_already_on);
    HU_RUN_TEST(spoken_turn_begin_off_and_shadow_leave_agent_alone);
    HU_RUN_TEST(spoken_turn_lean_head_has_fewer_examples_and_spoken_directive);
}
