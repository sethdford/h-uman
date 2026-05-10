#include "human/agent.h"
#include "human/agent/humanness.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

/* ── Build turn context: basic functionality ─────────────────────────── */

static void build_context_null_agent_returns_error(void) {
    HU_ASSERT_EQ(hu_agent_build_turn_context(NULL), HU_ERR_INVALID_ARGUMENT);
}

static void build_context_skips_when_already_set(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.conversation_context = "existing context";
    agent.conversation_context_len = 16;

    hu_error_t err = hu_agent_build_turn_context(&agent);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(agent.conversation_context, "existing context");
    HU_ASSERT_EQ(agent.humanness_ctx_owned, false);
}

static void build_context_produces_empty_without_persona(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;

    hu_error_t err = hu_agent_build_turn_context(&agent);
    HU_ASSERT_EQ(err, HU_OK);
    hu_agent_free_turn_context(&agent);
}

/* ── Free turn context ───────────────────────────────────────────────── */

static void free_context_null_safe(void) {
    hu_agent_free_turn_context(NULL);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.humanness_ctx_owned = false;
    hu_agent_free_turn_context(&agent);
}

static void free_context_clears_owned(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *buf = (char *)alloc.alloc(alloc.ctx, 32);
    memcpy(buf, "test context", 12);
    buf[12] = '\0';

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.conversation_context = buf;
    agent.conversation_context_len = 12;
    agent.humanness_ctx_owned = true;
    /* humanness_ctx_buf must be set whenever humanness_ctx_owned is true
     * (this is the new contract enforced by hu_agent_build_turn_context). */
    agent.humanness_ctx_buf = buf;
    agent.humanness_ctx_buf_len = 12;

    hu_agent_free_turn_context(&agent);
    HU_ASSERT_NULL(agent.conversation_context);
    HU_ASSERT_EQ(agent.conversation_context_len, 0u);
    HU_ASSERT_EQ(agent.humanness_ctx_owned, false);
    HU_ASSERT(agent.humanness_ctx_buf == NULL);
}

static void free_context_does_not_free_external(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.conversation_context = "daemon owned";
    agent.conversation_context_len = 12;
    agent.humanness_ctx_owned = false;

    hu_agent_free_turn_context(&agent);
    HU_ASSERT_STR_EQ(agent.conversation_context, "daemon owned");
    HU_ASSERT_EQ(agent.conversation_context_len, 12u);
}

/* Regression for the production double-free caught by ASan on 2026-05-10:
 *
 *   1) humanness builds context on turn N, sets conversation_context=A and
 *      humanness_ctx_owned=true.
 *   2) hu_agent_free_turn_context is called by the agent on turn N+1, and
 *      ALSO frees A (correct).
 *   3) BUT — production daemon overrides agent->conversation_context = B
 *      (its own buffer) BEFORE turn N+1 runs, without clearing the
 *      humanness_ctx_owned flag. The next free_turn_context then frees B,
 *      and the daemon double-frees B at end-of-turn cleanup.
 *
 * Fix contract: free_turn_context must only free what humanness allocated.
 * If the conversation_context has been overwritten externally, the
 * ownership flag is reset and no free happens — daemon retains ownership.
 */
static void free_context_handles_daemon_override_after_humanness_owned(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Step 1: humanness "builds" context (we simulate by allocating). */
    char *humanness_buf = (char *)alloc.alloc(alloc.ctx, 32);
    HU_ASSERT(humanness_buf != NULL);
    memcpy(humanness_buf, "humanness ctx", 13);
    humanness_buf[13] = '\0';

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.conversation_context = humanness_buf;
    agent.conversation_context_len = 13;
    agent.humanness_ctx_owned = true;
    agent.humanness_ctx_buf = humanness_buf;
    agent.humanness_ctx_buf_len = 13;

    /* Step 2: daemon allocates its own buffer and overrides without
     * clearing humanness_ctx_owned (this is the production bug shape). */
    char *daemon_buf = (char *)alloc.alloc(alloc.ctx, 64);
    HU_ASSERT(daemon_buf != NULL);
    memcpy(daemon_buf, "daemon convo_ctx", 16);
    daemon_buf[16] = '\0';
    agent.conversation_context = daemon_buf;
    agent.conversation_context_len = 16;
    /* humanness_ctx_owned still true; humanness_ctx_buf still points at the
     * humanness allocation we made in step 1. */

    /* Step 3: free_turn_context fires (called from agent_stream/turn).
     * Must free humanness_buf (which we still own through humanness_ctx_buf)
     * but must NOT touch daemon_buf. */
    hu_agent_free_turn_context(&agent);

    HU_ASSERT_EQ(agent.humanness_ctx_owned, false);
    HU_ASSERT(agent.humanness_ctx_buf == NULL);
    /* daemon_buf survives — its content is intact and we still own it. */
    HU_ASSERT(agent.conversation_context == daemon_buf);
    HU_ASSERT_STR_EQ(daemon_buf, "daemon convo_ctx");

    /* Daemon cleans up its own buffer — single free, no ASan trip. */
    alloc.free(alloc.ctx, daemon_buf, 64);
}

/* Twin regression: when humanness owns AND the daemon never overrode,
 * free_turn_context still frees correctly via the ownership pointer. */
static void free_context_frees_humanness_owned_via_buf_pointer(void) {
    hu_allocator_t alloc = hu_system_allocator();

    char *humanness_buf = (char *)alloc.alloc(alloc.ctx, 32);
    HU_ASSERT(humanness_buf != NULL);
    memcpy(humanness_buf, "humanness only", 14);
    humanness_buf[14] = '\0';

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.conversation_context = humanness_buf;
    agent.conversation_context_len = 14;
    agent.humanness_ctx_owned = true;
    agent.humanness_ctx_buf = humanness_buf;
    agent.humanness_ctx_buf_len = 14;

    hu_agent_free_turn_context(&agent);

    HU_ASSERT_NULL(agent.conversation_context);
    HU_ASSERT_EQ(agent.conversation_context_len, 0u);
    HU_ASSERT_EQ(agent.humanness_ctx_owned, false);
    HU_ASSERT(agent.humanness_ctx_buf == NULL);
}

/* ── Voice profile update ─────────────────────────────────────────────── */

static void voice_update_null_agent_returns_error(void) {
    HU_ASSERT_EQ(hu_agent_update_voice_profile(NULL, "hello", 5), HU_ERR_INVALID_ARGUMENT);
}

static void voice_update_uninitialized_returns_error(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.voice_profile_initialized = false;
    HU_ASSERT_EQ(hu_agent_update_voice_profile(&agent, "hello", 5), HU_ERR_INVALID_ARGUMENT);
}

static void voice_update_increments_interaction(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_voice_profile_init(&agent.voice_profile);
    agent.voice_profile_initialized = true;

    uint32_t before = agent.voice_profile.interaction_count;
    hu_agent_update_voice_profile(&agent, "hey how are you", 15);
    HU_ASSERT(agent.voice_profile.interaction_count > before);
}

static void voice_update_detects_emotion(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_voice_profile_init(&agent.voice_profile);
    agent.voice_profile_initialized = true;

    uint32_t before = agent.voice_profile.emotional_exchanges;
    hu_agent_update_voice_profile(&agent, "I feel so sad about this", 24);
    HU_ASSERT(agent.voice_profile.emotional_exchanges > before);
}

static void voice_update_detects_topic(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_voice_profile_init(&agent.voice_profile);
    agent.voice_profile_initialized = true;

    const char *long_msg =
        "I was thinking about the implications of quantum computing on cryptography and whether "
        "post-quantum encryption methods will be ready in time";
    uint32_t before = agent.voice_profile.shared_topics;
    hu_agent_update_voice_profile(&agent, long_msg, strlen(long_msg));
    HU_ASSERT(agent.voice_profile.shared_topics > before);
}

static void voice_profile_stages_evolve(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_voice_profile_init(&agent.voice_profile);
    agent.voice_profile_initialized = true;

    HU_ASSERT_EQ(agent.voice_profile.stage, HU_VOICE_FORMAL);
    for (int i = 0; i < 20; i++)
        hu_agent_update_voice_profile(&agent, "I feel happy about this topic discussion", 40);
    HU_ASSERT(agent.voice_profile.stage > HU_VOICE_FORMAL);
}

/* ── Registration ─────────────────────────────────────────────────────── */

void run_humanness_context_tests(void) {
    HU_TEST_SUITE("Humanness Context");

    HU_RUN_TEST(build_context_null_agent_returns_error);
    HU_RUN_TEST(build_context_skips_when_already_set);
    HU_RUN_TEST(build_context_produces_empty_without_persona);
    HU_RUN_TEST(free_context_null_safe);
    HU_RUN_TEST(free_context_clears_owned);
    HU_RUN_TEST(free_context_does_not_free_external);
    HU_RUN_TEST(free_context_handles_daemon_override_after_humanness_owned);
    HU_RUN_TEST(free_context_frees_humanness_owned_via_buf_pointer);
    HU_RUN_TEST(voice_update_null_agent_returns_error);
    HU_RUN_TEST(voice_update_uninitialized_returns_error);
    HU_RUN_TEST(voice_update_increments_interaction);
    HU_RUN_TEST(voice_update_detects_emotion);
    HU_RUN_TEST(voice_update_detects_topic);
    HU_RUN_TEST(voice_profile_stages_evolve);
}
