/* test_outbound_echo.c — Sprint 59 echo stage contract.
 *
 * Echo detects directive-echo failures. Two outcomes:
 *   - REJECT  on standalone-directive strings ("shared history",
 *             "principle", "under 10 words")
 *   - REGENERATE on >= 40% token overlap with the prompt_used
 *
 * Corpus coverage: #5 (long prompt-echo), #7, #8, #9, #10.
 */

#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#include "human/agent/outbound_pipeline.h"
#include "human/core/allocator.h"

extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_echo;

static hu_allocator_t *test_alloc(void) {
    static hu_allocator_t a;
    static int init = 0;
    if (!init) {
        a = hu_system_allocator();
        init = 1;
    }
    return &a;
}

static hu_outbound_verdict_t run_echo(const char *content, const char *prompt) {
    hu_outbound_message_t msg = {0};
    msg.content = (char *)content;
    msg.content_len = strlen(content);
    msg.prompt_used = prompt;
    msg.prompt_used_len = prompt ? strlen(prompt) : 0;

    hu_outbound_context_t ctx = {0};
    ctx.alloc = test_alloc();
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.regenerate_budget = 1;

    return hu_outbound_pipeline_stage_echo.run(&hu_outbound_pipeline_stage_echo, &msg, &ctx);
}

/* ----------------------------------------------------------------- */
/* Standalone-directive REJECT cases                                 */
/* ----------------------------------------------------------------- */

static void test_echo_corpus_shared_history_rejects(void) {
    /* Corpus #7, #8. */
    hu_outbound_verdict_t v = run_echo("shared history", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
    HU_ASSERT_STR_EQ(v.reason, "echo_standalone_directive");
}

static void test_echo_corpus_principle_rejects(void) {
    /* Corpus #9. */
    hu_outbound_verdict_t v = run_echo("principle", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
}

static void test_echo_corpus_under_10_words_rejects(void) {
    /* Corpus #10. */
    hu_outbound_verdict_t v = run_echo("under 10 words", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
}

/* Trim whitespace + punctuation before matching. */
static void test_echo_directive_with_trailing_period_rejects(void) {
    hu_outbound_verdict_t v = run_echo("principle.", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
}

static void test_echo_directive_with_surrounding_whitespace_rejects(void) {
    hu_outbound_verdict_t v = run_echo("  shared history  ", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
}

/* Case-insensitive match. */
static void test_echo_directive_case_insensitive_rejects(void) {
    hu_outbound_verdict_t v = run_echo("Shared History", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
}

/* ----------------------------------------------------------------- */
/* Prompt-overlap REGENERATE cases                                   */
/* ----------------------------------------------------------------- */

static void test_echo_corpus_5_directive_prefix_rejects(void) {
    /* Corpus #5: LLM echoed the prompt verbatim, starting with the
     * directive prefix "reference something specific". Algorithm 1b
     * (directive-prefix REJECT) fires before Algorithm 2 (prompt-
     * overlap REGENERATE) — REJECT is the stronger verdict for
     * verbatim instruction echo. */
    const char *content = "reference something specific you know about them or ask about "
                          "something from a previous conversation";
    const char *prompt =
        "When writing a check-in to a contact, reference something specific you know "
        "about them or ask about something from a previous conversation. Keep it short, "
        "under 10 words, like Seth would write.";
    hu_outbound_verdict_t v = run_echo(content, prompt);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
    HU_ASSERT_STR_EQ(v.reason, "echo_directive_prefix");
}

/* Pure prompt-overlap path (no directive-prefix match). Crafted to
 * not start with any DIRECTIVE_PREFIXES entry. */
static void test_echo_high_overlap_no_prefix_regenerates(void) {
    const char *content = "the garden the meeting the loan tomorrow";
    const char *prompt = "Topics: the garden the meeting the loan tomorrow";
    hu_outbound_verdict_t v = run_echo(content, prompt);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
    HU_ASSERT_STR_EQ(v.reason, "echo_prompt_overlap");
}

static void test_echo_legitimate_message_low_overlap_sends(void) {
    const char *prompt =
        "When writing a check-in to a contact, reference something specific you know "
        "about them or ask about something from a previous conversation. Keep it short.";
    hu_outbound_verdict_t v = run_echo("how's the garden doing?", prompt);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

static void test_echo_no_prompt_provided_skips_overlap_check(void) {
    /* Without a prompt_used field, overlap check cannot run. The
     * stage falls through to SEND (unless standalone directive). */
    hu_outbound_verdict_t v = run_echo("how are those funny looking dogs doing?", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* ----------------------------------------------------------------- */
/* Negative-space tests                                              */
/* ----------------------------------------------------------------- */

static void test_echo_empty_returns_send(void) {
    hu_outbound_verdict_t v = run_echo("", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

static void test_echo_whitespace_only_returns_send(void) {
    hu_outbound_verdict_t v = run_echo("   \t\n  ", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

static void test_echo_corpus_pass_cases_send(void) {
    const char *prompt =
        "When writing a check-in to a contact, reference something specific you know "
        "about them or ask about something from a previous conversation.";
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
        hu_outbound_verdict_t v = run_echo(pass_cases[i], prompt);
        HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
    }
}

/* Word "reference" alone — exact directive match → REJECT. But
 * "give me a reference on that" should NOT trigger (only standalone). */
static void test_echo_reference_in_sentence_not_rejected_by_directive(void) {
    hu_outbound_verdict_t v = run_echo("can you give me a reference on that?", NULL);
    /* Not a standalone directive; without prompt, no overlap check.
     * Should pass. */
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

void run_outbound_echo_tests(void) {
    HU_TEST_SUITE("outbound_echo");
    HU_RUN_TEST(test_echo_corpus_shared_history_rejects);
    HU_RUN_TEST(test_echo_corpus_principle_rejects);
    HU_RUN_TEST(test_echo_corpus_under_10_words_rejects);
    HU_RUN_TEST(test_echo_directive_with_trailing_period_rejects);
    HU_RUN_TEST(test_echo_directive_with_surrounding_whitespace_rejects);
    HU_RUN_TEST(test_echo_directive_case_insensitive_rejects);
    HU_RUN_TEST(test_echo_corpus_5_directive_prefix_rejects);
    HU_RUN_TEST(test_echo_high_overlap_no_prefix_regenerates);
    HU_RUN_TEST(test_echo_legitimate_message_low_overlap_sends);
    HU_RUN_TEST(test_echo_no_prompt_provided_skips_overlap_check);
    HU_RUN_TEST(test_echo_empty_returns_send);
    HU_RUN_TEST(test_echo_whitespace_only_returns_send);
    HU_RUN_TEST(test_echo_corpus_pass_cases_send);
    HU_RUN_TEST(test_echo_reference_in_sentence_not_rejected_by_directive);
}
