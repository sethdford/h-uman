/* Voice-first memos (spec 2026-09-28): the daemon decides voice before the
 * turn and, LIVE and on the family list, has the turn write a memo. */
#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/daemon/voice_first.h"
#include "human/persona.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *ctx;
    size_t ctx_len;
    uint32_t max_chars;
    hu_daemon_voice_first_t vf;
} vf_run_t;

static bool g_group;

static void run(const char *mode, const char *allow, const char *inbound, vf_run_t *r) {
    hu_allocator_t alloc = hu_system_allocator();
    static hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    snprintf(persona.voice.voice_id, sizeof(persona.voice.voice_id), "test-voice");
    persona.voice_messages.enabled = true;
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.persona = &persona;
    if (mode)
        setenv("HU_VOICE_FIRST", mode, 1);
    else
        unsetenv("HU_VOICE_FIRST");
    if (allow)
        setenv("HU_VOICE_DELIVERY_ONLY", allow, 1);
    else
        unsetenv("HU_VOICE_DELIVERY_ONLY");
    const char *base = "[earlier] they asked about the lake";
    r->ctx_len = strlen(base);
    r->ctx = alloc.alloc(alloc.ctx, r->ctx_len + 1);
    memcpy(r->ctx, base, r->ctx_len + 1);
    r->max_chars = 200;
    hu_daemon_voice_first_prepare(&alloc, &agent, "+15550000001", 12, g_group, inbound,
                                  strlen(inbound), &r->ctx, &r->ctx_len, &r->max_chars, &r->vf);
    unsetenv("HU_VOICE_FIRST");
    unsetenv("HU_VOICE_DELIVERY_ONLY");
}

static void done(vf_run_t *r) {
    hu_allocator_t alloc = hu_system_allocator();
    alloc.free(alloc.ctx, r->ctx, r->ctx_len + 1);
}

static void test_voice_first_off_changes_nothing(void) {
    vf_run_t r;
    run(NULL, "+15550000001", "I'm so proud of you", &r);
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_EQ(r.max_chars, 200);
    HU_ASSERT_STR_EQ(r.ctx, "[earlier] they asked about the lake");
    done(&r);
}

static void test_voice_first_live_writes_a_memo_for_family(void) {
    vf_run_t r;
    run("live", "+15550000009,+15550000001", "I'm so proud of you", &r);
    HU_ASSERT_TRUE(r.vf.memo);
    HU_ASSERT_STR_EQ(r.vf.reason, "heartfelt");
    HU_ASSERT_EQ(r.max_chars, HU_VOICE_FIRST_MEMO_MAX_CHARS);
    HU_ASSERT_TRUE(strncmp(r.ctx, "VOICE MEMO:", 11) == 0);
    HU_ASSERT_STR_CONTAINS(r.ctx, "[earlier] they asked about the lake");
    HU_ASSERT_STR_CONTAINS(r.ctx, "wonderful day"); /* named as what NOT to say */
    done(&r);
}

static void test_voice_first_live_leaves_others_as_text(void) {
    vf_run_t r;
    run("live", "+15550000009", "I'm so proud of you", &r);
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_EQ((int)r.vf.decision, (int)HU_VOICE_SEND_VOICE); /* decided, not eligible */
    HU_ASSERT_EQ(r.max_chars, 200);
    done(&r);
    run("live", NULL, "I'm so proud of you", &r); /* no list: nobody */
    HU_ASSERT_FALSE(r.vf.memo);
    done(&r);
}

static void test_voice_first_shadow_decides_but_writes_text(void) {
    vf_run_t r;
    run("shadow", "+15550000001", "[Audio transcription: miss you guys]", &r);
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_EQ((int)r.vf.decision, (int)HU_VOICE_SEND_VOICE);
    HU_ASSERT_STR_EQ(r.vf.reason, "they_sent_audio");
    HU_ASSERT_STR_EQ(r.ctx, "[earlier] they asked about the lake");
    done(&r);
}

static void test_voice_first_live_without_a_trigger_stays_text(void) {
    vf_run_t r;
    run("live", "+15550000001", "ok sounds good", &r);
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_STR_EQ(r.vf.reason, "no_trigger");
    HU_ASSERT_EQ(r.max_chars, 200);
    done(&r);
}

static void test_voice_first_stands_down_in_group_chats(void) {
    vf_run_t r;
    g_group = true;
    run("live", "+15550000001", "I'm so proud of you", &r);
    g_group = false;
    HU_ASSERT_FALSE(r.vf.memo);
    HU_ASSERT_STR_EQ(r.vf.reason, "group");
    HU_ASSERT_EQ(r.max_chars, 200);
    done(&r);
}

/* What the director is told: voice is on the table only where voice-first
 * LIVE would actually send one (family list, not a group, a voice set up). */
static void test_voice_first_available_for_the_director(void) {
    static hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    snprintf(persona.voice.voice_id, sizeof(persona.voice.voice_id), "test-voice");
    persona.voice_messages.enabled = true;
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.persona = &persona;
    setenv("HU_VOICE_DELIVERY_ONLY", "+15550000001", 1);
    setenv("HU_VOICE_FIRST", "live", 1);
    HU_ASSERT_TRUE(hu_daemon_voice_first_available(&agent, "+15550000001", 12, false));
    HU_ASSERT_FALSE(hu_daemon_voice_first_available(&agent, "+15550000001", 12, true));
    HU_ASSERT_FALSE(hu_daemon_voice_first_available(&agent, "+15550000002", 12, false));
    setenv("HU_VOICE_FIRST", "shadow", 1);
    HU_ASSERT_FALSE(hu_daemon_voice_first_available(&agent, "+15550000001", 12, false));
    unsetenv("HU_VOICE_FIRST");
    unsetenv("HU_VOICE_DELIVERY_ONLY");
}

void run_daemon_voice_first_tests(void) {
    HU_TEST_SUITE("daemon voice-first memos");
    HU_RUN_TEST(test_voice_first_off_changes_nothing);
    HU_RUN_TEST(test_voice_first_available_for_the_director);
    HU_RUN_TEST(test_voice_first_stands_down_in_group_chats);
    HU_RUN_TEST(test_voice_first_live_writes_a_memo_for_family);
    HU_RUN_TEST(test_voice_first_live_leaves_others_as_text);
    HU_RUN_TEST(test_voice_first_shadow_decides_but_writes_text);
    HU_RUN_TEST(test_voice_first_live_without_a_trigger_stays_text);
}
