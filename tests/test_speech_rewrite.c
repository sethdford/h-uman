/* tests/test_speech_rewrite.c
 *
 * F1 S1: rewrite a voice reply for the ear (Ferni mechanics, the persona's
 * own register), behind HU_SPEECH_REWRITE off|shadow|live. The provider is a
 * mock; nothing touches the network. */
#include "human/core/allocator.h"
#include "human/persona.h"
#include "human/provider.h"
#include "human/tts/speech_rewrite.h"
#include "test_framework.h"

#include <string.h>

typedef struct {
    int calls;
    const char *reply; /* NULL -> empty output */
    hu_error_t err;
    char last_sys[4096];
    char last_msg[2048];
} mock_llm_t;

static hu_error_t mock_chat(void *ctx, hu_allocator_t *alloc, const char *sys, size_t sys_len,
                            const char *msg, size_t msg_len, const char *model, size_t model_len,
                            double temperature, char **out, size_t *out_len) {
    (void)model;
    (void)model_len;
    (void)temperature;
    mock_llm_t *m = ctx;
    m->calls++;
    snprintf(m->last_sys, sizeof(m->last_sys), "%.*s", (int)sys_len, sys);
    snprintf(m->last_msg, sizeof(m->last_msg), "%.*s", (int)msg_len, msg);
    *out = NULL;
    *out_len = 0;
    if (m->err != HU_OK)
        return m->err;
    if (!m->reply)
        return HU_OK;
    size_t n = strlen(m->reply);
    char *b = alloc->alloc(alloc->ctx, n + 1);
    memcpy(b, m->reply, n + 1);
    *out = b;
    *out_len = n;
    return HU_OK;
}

static hu_provider_vtable_t g_vt;

static hu_provider_t mock_provider(mock_llm_t *m) {
    memset(&g_vt, 0, sizeof(g_vt));
    g_vt.chat_with_system = mock_chat;
    hu_provider_t p = {.ctx = m, .vtable = &g_vt};
    return p;
}

static char *g_slang[] = {"dude", "legit"};

static hu_persona_t test_persona(void) {
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    p.slang = g_slang;
    p.slang_count = 2;
    return p;
}

static hu_error_t run(mock_llm_t *m, hu_speech_rewrite_mode_t mode, const char *reply,
                      const char *inbound, hu_speech_result_t *out) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = mock_provider(m);
    hu_persona_t persona = test_persona();
    return hu_speech_prepare(&alloc, m ? &prov : NULL, "test-model", 10, &persona, mode, reply,
                             strlen(reply), inbound, inbound ? strlen(inbound) : 0, out);
}

static void test_speech_rewrite_mode_parse(void) {
    HU_ASSERT_EQ(hu_speech_rewrite_mode_parse(NULL), HU_SPEECH_REWRITE_OFF);
    HU_ASSERT_EQ(hu_speech_rewrite_mode_parse("on"), HU_SPEECH_REWRITE_OFF);
    HU_ASSERT_EQ(hu_speech_rewrite_mode_parse("shadow"), HU_SPEECH_REWRITE_SHADOW);
    HU_ASSERT_EQ(hu_speech_rewrite_mode_parse("live"), HU_SPEECH_REWRITE_LIVE);
}

static void test_speech_rewrite_off_is_cleanup_only(void) {
    mock_llm_t m = {0};
    m.reply = "Should never be used.";
    hu_speech_result_t r;
    HU_ASSERT_EQ(run(&m, HU_SPEECH_REWRITE_OFF, "lmk when ur free", "hey", &r), HU_OK);
    HU_ASSERT_EQ(m.calls, 0);
    HU_ASSERT_STR_EQ(r.spoken, "let me know when you're free");
    HU_ASSERT_FALSE(r.used_rewrite);
    HU_ASSERT_STR_EQ(r.reason, "off");
}

static void test_speech_rewrite_live_speaks_a_faithful_rewrite(void) {
    mock_llm_t m = {0};
    m.reply = "Yeah, sounds good. Let me know.";
    hu_speech_result_t r;
    HU_ASSERT_EQ(run(&m, HU_SPEECH_REWRITE_LIVE, "yeah sounds good lmk", "dinner?", &r), HU_OK);
    HU_ASSERT_EQ(m.calls, 1);
    HU_ASSERT_STR_EQ(r.spoken, "Yeah, sounds good. Let me know.");
    HU_ASSERT_TRUE(r.used_rewrite);
}

static void test_speech_rewrite_live_rejects_invented_detail(void) {
    mock_llm_t m = {0};
    m.reply = "See you tomorrow at 7!"; /* the reply never named a time */
    hu_speech_result_t r;
    HU_ASSERT_EQ(run(&m, HU_SPEECH_REWRITE_LIVE, "see you tomorrow", "you coming?", &r), HU_OK);
    HU_ASSERT_STR_EQ(r.spoken, "see you tomorrow");
    HU_ASSERT_FALSE(r.used_rewrite);
    HU_ASSERT_STR_EQ(r.reason, "new_number");
}

static void test_speech_rewrite_shadow_calls_but_speaks_cleanup(void) {
    mock_llm_t m = {0};
    m.reply = "Yeah, sounds good. Let me know.";
    hu_speech_result_t r;
    HU_ASSERT_EQ(run(&m, HU_SPEECH_REWRITE_SHADOW, "yeah sounds good lmk", "dinner?", &r), HU_OK);
    HU_ASSERT_EQ(m.calls, 1);
    HU_ASSERT_STR_EQ(r.spoken, "yeah sounds good let me know");
    HU_ASSERT_FALSE(r.used_rewrite);
    HU_ASSERT_STR_EQ(r.reason, "shadow");
    HU_ASSERT_STR_EQ(r.rewritten, "Yeah, sounds good. Let me know.");
}

static void test_speech_rewrite_provider_failures_fall_back(void) {
    mock_llm_t m = {0};
    m.err = HU_ERR_IO;
    hu_speech_result_t r;
    HU_ASSERT_EQ(run(&m, HU_SPEECH_REWRITE_LIVE, "sounds good", "ok?", &r), HU_OK);
    HU_ASSERT_STR_EQ(r.spoken, "sounds good");
    HU_ASSERT_STR_EQ(r.reason, "provider_error");
    mock_llm_t e = {0}; /* empty output */
    HU_ASSERT_EQ(run(&e, HU_SPEECH_REWRITE_LIVE, "sounds good", "ok?", &r), HU_OK);
    HU_ASSERT_STR_EQ(r.spoken, "sounds good");
    HU_ASSERT_STR_EQ(r.reason, "empty");
    HU_ASSERT_EQ(run(NULL, HU_SPEECH_REWRITE_LIVE, "sounds good", "ok?", &r), HU_OK);
    HU_ASSERT_STR_EQ(r.reason, "no_provider");
}

static void test_speech_rewrite_output_is_cleaned_too(void) {
    mock_llm_t m = {0};
    m.reply = "\"Sounds good lol\""; /* quotes and a laugh token from the model */
    hu_speech_result_t r;
    HU_ASSERT_EQ(run(&m, HU_SPEECH_REWRITE_LIVE, "sounds good", "ok?", &r), HU_OK);
    HU_ASSERT_STR_EQ(r.spoken, "Sounds good");
    HU_ASSERT_TRUE(r.laughter_cue);
}

static void test_speech_rewrite_nothing_speakable_is_empty(void) {
    mock_llm_t m = {0};
    hu_speech_result_t r;
    HU_ASSERT_EQ(run(&m, HU_SPEECH_REWRITE_LIVE, "\xF0\x9F\x91\x8D", "ok?", &r), HU_OK);
    HU_ASSERT_EQ(r.spoken_len, 0);
    HU_ASSERT_EQ(m.calls, 0); /* no call when there is nothing to say */
}

static void test_speech_rewrite_prompt_carries_mechanics_persona_and_context(void) {
    mock_llm_t m = {0};
    m.reply = "Yeah, sounds good.";
    hu_speech_result_t r;
    HU_ASSERT_EQ(run(&m, HU_SPEECH_REWRITE_LIVE, "yeah sounds good", "dinner at mine?", &r), HU_OK);
    HU_ASSERT_STR_CONTAINS(m.last_sys, "out loud");
    HU_ASSERT_STR_CONTAINS(m.last_sys, "Never start with");
    HU_ASSERT_STR_CONTAINS(m.last_sys, "dude");
    HU_ASSERT_TRUE(strlen(m.last_sys) < 4095);
    HU_ASSERT_STR_CONTAINS(m.last_msg, "dinner at mine?");
    HU_ASSERT_STR_CONTAINS(m.last_msg, "yeah sounds good");
}

static void test_speech_rewrite_shadow_line_is_json_without_inbound(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_speech_result_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.spoken, sizeof(r.spoken), "sounds good");
    r.spoken_len = strlen(r.spoken);
    snprintf(r.rewritten, sizeof(r.rewritten), "He said \"hi\"");
    r.reason = "shadow";
    char *line = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_speech_shadow_line(&alloc, &r, 1790000000, &line, &len), HU_OK);
    HU_ASSERT_NOT_NULL(line);
    HU_ASSERT_STR_CONTAINS(line, "\\\"hi\\\"");
    HU_ASSERT_STR_CONTAINS(line, "\"reason\":\"shadow\"");
    HU_ASSERT_TRUE(line[0] == '{' && line[len - 1] == '}');
    alloc.free(alloc.ctx, line, len + 1);
}

/* voiceai 2026-09-27 ("write for the ear"): joined thoughts, not a run of
 * short sentences. */
static void test_speech_rewrite_prompt_writes_for_the_ear(void) {
    char sys[4096];
    HU_ASSERT_TRUE(hu_speech_rewrite_system_prompt(NULL, sys, sizeof(sys)) > 0);
    HU_ASSERT_STR_CONTAINS(sys, "and, so, but, because");
    HU_ASSERT_STR_NOT_CONTAINS(sys, "short sentences; fragments");
}

void run_speech_rewrite_tests(void) {
    HU_TEST_SUITE("speech rewrite (F1 S1)");
    HU_RUN_TEST(test_speech_rewrite_prompt_writes_for_the_ear);
    HU_RUN_TEST(test_speech_rewrite_mode_parse);
    HU_RUN_TEST(test_speech_rewrite_off_is_cleanup_only);
    HU_RUN_TEST(test_speech_rewrite_live_speaks_a_faithful_rewrite);
    HU_RUN_TEST(test_speech_rewrite_live_rejects_invented_detail);
    HU_RUN_TEST(test_speech_rewrite_shadow_calls_but_speaks_cleanup);
    HU_RUN_TEST(test_speech_rewrite_provider_failures_fall_back);
    HU_RUN_TEST(test_speech_rewrite_output_is_cleaned_too);
    HU_RUN_TEST(test_speech_rewrite_nothing_speakable_is_empty);
    HU_RUN_TEST(test_speech_rewrite_prompt_carries_mechanics_persona_and_context);
    HU_RUN_TEST(test_speech_rewrite_shadow_line_is_json_without_inbound);
}
