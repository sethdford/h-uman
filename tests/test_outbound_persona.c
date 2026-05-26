/* test_outbound_persona.c — Sprint 59 persona stage contract.
 *
 * Persona detects two failure modes:
 *   - Project jargon in outbound (corpus #11-16: Replay MCP)
 *   - AI-self-aware narration (corpus #17-18: BORDERLINE)
 *
 * Both REGENERATE — never REJECT (per Q-3 user decision).
 * PASS cases (#19-24) remain SEND.
 */

#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#include "human/agent/outbound_pipeline.h"
#include "human/core/allocator.h"

extern hu_outbound_stage_t hu_outbound_stage_persona;

static hu_allocator_t *test_alloc(void) {
    static hu_allocator_t a;
    static int init = 0;
    if (!init) {
        a = hu_system_allocator();
        init = 1;
    }
    return &a;
}

static hu_outbound_verdict_t run_persona(const char *content) {
    hu_outbound_message_t msg = {0};
    msg.content = (char *)content;
    msg.content_len = strlen(content);

    hu_outbound_context_t ctx = {0};
    ctx.alloc = test_alloc();
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.regenerate_budget = 1;

    return hu_outbound_stage_persona.run(&hu_outbound_stage_persona, &msg, &ctx);
}

/* ----------------------------------------------------------------- */
/* Project-jargon REGENERATE — corpus #11-16                        */
/* ----------------------------------------------------------------- */

static void test_persona_corpus_11_replay_mcp_regenerates(void) {
    /* "finally got that Replay MCP stuff ready" */
    hu_outbound_verdict_t v = run_persona("finally got that Replay MCP stuff ready");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
    HU_ASSERT_STR_EQ(v.reason, "persona_project_jargon");
}

static void test_persona_corpus_12_morning_replay_mcp_regenerates(void) {
    hu_outbound_verdict_t v = run_persona("morning! How's that Replay MCP stuff coming along?");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
}

static void test_persona_corpus_15_lowercase_regenerates(void) {
    hu_outbound_verdict_t v = run_persona("how's that replay mcp coming along?");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
}

static void test_persona_lora_jargon_regenerates(void) {
    hu_outbound_verdict_t v = run_persona("just trained a new LoRA adapter on Seth-style chats");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
}

static void test_persona_checkpoint_jargon_regenerates(void) {
    hu_outbound_verdict_t v = run_persona("got the new checkpoint loaded and running great");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
}

/* ----------------------------------------------------------------- */
/* AI-self-aware REGENERATE — corpus #17, #18                       */
/* ----------------------------------------------------------------- */

static void test_persona_corpus_17_quiet_lately_regenerates(void) {
    /* Corpus #17, #18: "I've been kind of quiet lately" */
    hu_outbound_verdict_t v = run_persona("I've been kind of quiet lately, just checking in");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
    HU_ASSERT_STR_EQ(v.reason, "persona_ai_self_aware");
}

static void test_persona_as_your_assistant_regenerates(void) {
    hu_outbound_verdict_t v = run_persona("as your assistant I wanted to remind you");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
}

static void test_persona_let_me_know_regenerates(void) {
    hu_outbound_verdict_t v = run_persona("let me know if you'd like me to follow up");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
}

/* ----------------------------------------------------------------- */
/* PASS-case regression                                              */
/* ----------------------------------------------------------------- */

static void test_persona_corpus_pass_cases_send(void) {
    const char *pass_cases[] = {
        "how'd it go with the loan?",
        "you still getting that loan tomorrow?",
        "morning! How's the garden doing?",
        "how are those funny looking dogs doing?",
        "see any more funny looking dogs lately?",
        "how are you",
        NULL,
    };
    for (int i = 0; pass_cases[i]; i++) {
        hu_outbound_verdict_t v = run_persona(pass_cases[i]);
        HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
    }
}

/* ----------------------------------------------------------------- */
/* Edge cases                                                        */
/* ----------------------------------------------------------------- */

static void test_persona_empty_returns_send(void) {
    hu_outbound_verdict_t v = run_persona("");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

static void test_persona_null_content_returns_send(void) {
    hu_outbound_message_t msg = {0};
    hu_outbound_context_t ctx = {0};
    ctx.alloc = test_alloc();
    hu_outbound_verdict_t v = hu_outbound_stage_persona.run(&hu_outbound_stage_persona, &msg, &ctx);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* "mcp" inside a word should NOT trigger ("mcpherson"). The list
 * uses bounded forms (" mcp ", " mcp.") to avoid this. */
static void test_persona_mcp_inside_word_sends(void) {
    hu_outbound_verdict_t v = run_persona("met with mcpherson today");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

void run_outbound_persona_tests(void) {
    HU_TEST_SUITE("outbound_persona");
    HU_RUN_TEST(test_persona_corpus_11_replay_mcp_regenerates);
    HU_RUN_TEST(test_persona_corpus_12_morning_replay_mcp_regenerates);
    HU_RUN_TEST(test_persona_corpus_15_lowercase_regenerates);
    HU_RUN_TEST(test_persona_lora_jargon_regenerates);
    HU_RUN_TEST(test_persona_checkpoint_jargon_regenerates);
    HU_RUN_TEST(test_persona_corpus_17_quiet_lately_regenerates);
    HU_RUN_TEST(test_persona_as_your_assistant_regenerates);
    HU_RUN_TEST(test_persona_let_me_know_regenerates);
    HU_RUN_TEST(test_persona_corpus_pass_cases_send);
    HU_RUN_TEST(test_persona_empty_returns_send);
    HU_RUN_TEST(test_persona_null_content_returns_send);
    HU_RUN_TEST(test_persona_mcp_inside_word_sends);
}
