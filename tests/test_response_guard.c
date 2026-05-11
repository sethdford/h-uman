/* Tests for hu_response_guard. The first two tests are the EXACT production
 * failure that landed at a real human contact on 2026-05-10:
 *
 *   "Like <|channel>thoughtThe user said said \"Here! \" \" \" \" \" \" "
 *   \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
 *   ...repeating ~250 times..."
 *
 * Both classes (special-token leak + degenerate repetition) must be caught.
 */
#include "human/agent/response_guard.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static hu_allocator_t A(void) { return hu_system_allocator(); }

/* ── Production regression — Harmony channel marker ────────────────────── */

static void guard_rejects_or_sanitizes_production_harmony_leak(void) {
    /* Reproduces the exact wire-format leak. */
    const char *raw =
        "Like <|channel>thoughtThe user said said \"Here! \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \"";

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);

    /* Must not be HU_GUARD_OK — that would mean we'd ship this to a user. */
    HU_ASSERT(outcome != HU_GUARD_OK);

    /* Should be REJECTED (degenerate); even if it were merely rewritten,
     * the rewrite would be < 80 chars, not the 600+ byte original. */
    if (outcome == HU_GUARD_REWROTE) {
        HU_ASSERT(out != NULL);
        HU_ASSERT(out_len < 200u);
        /* Special tokens must not be in the output. */
        HU_ASSERT(strstr(out, "<|channel") == NULL);
        HU_ASSERT(strstr(out, "<|message") == NULL);
        HU_ASSERT(strstr(out, "<|thought") == NULL);
        alloc.free(alloc.ctx, out, out_len + 1);
    } else {
        /* HU_GUARD_REJECT */
        HU_ASSERT(out == NULL);
        HU_ASSERT_EQ(out_len, 0u);
    }

    HU_ASSERT(report.detected_degenerate_repetition);
}

static void guard_strips_harmony_channel_marker(void) {
    const char *raw = "Hey! <|channel|>final<|message|>I'd love that.";

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);

    HU_ASSERT_EQ(outcome, HU_GUARD_REWROTE);
    HU_ASSERT(out != NULL);
    HU_ASSERT(report.stripped_harmony_tokens);
    HU_ASSERT(strstr(out, "<|") == NULL);
    /* The actual message text survives. */
    HU_ASSERT(strstr(out, "Hey!") != NULL);
    HU_ASSERT(strstr(out, "I'd love that.") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void guard_strips_unclosed_channel_token(void) {
    /* The PROD leak had `<|channel>thought` — note the missing closing |>.
     * This often happens when the model emits its own header mid-stream. */
    const char *raw = "<|channel>thoughtSure thing!";

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);

    HU_ASSERT_EQ(outcome, HU_GUARD_REWROTE);
    HU_ASSERT(out != NULL);
    HU_ASSERT(report.stripped_harmony_tokens);
    HU_ASSERT(strstr(out, "<|channel") == NULL);
    HU_ASSERT(strstr(out, "Sure thing!") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void guard_strips_thinking_block(void) {
    const char *raw = "<think>I should ask about her day.</think>How was your day?";

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);

    HU_ASSERT_EQ(outcome, HU_GUARD_REWROTE);
    HU_ASSERT(report.stripped_thinking_block);
    HU_ASSERT(strstr(out, "<think>") == NULL);
    HU_ASSERT(strstr(out, "</think>") == NULL);
    HU_ASSERT(strstr(out, "How was your day?") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void guard_strips_capital_thought_block(void) {
    const char *raw = "<thought>plan: warm response</thought>Hey, missed you.";

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);

    HU_ASSERT_EQ(outcome, HU_GUARD_REWROTE);
    HU_ASSERT(report.stripped_thinking_block);
    HU_ASSERT(strstr(out, "<thought>") == NULL);
    HU_ASSERT(strstr(out, "Hey, missed you.") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void guard_strips_all_known_harmony_markers(void) {
    static const char *markers[] = {"<|start|>",       "<|end|>",       "<|return|>",
                                    "<|channel|>",     "<|message|>",   "<|thought|>",
                                    "<|user|>",        "<|assistant|>", "<|system|>",
                                    "<|tool_call|>",   "<|tool_response|>"};

    hu_allocator_t alloc = A();
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        char raw[128];
        snprintf(raw, sizeof(raw), "Hello %s world", markers[i]);
        char *out = NULL;
        size_t out_len = 0;
        hu_guard_outcome_t outcome = HU_GUARD_OK;
        hu_guard_report_t report;
        memset(&report, 0, sizeof(report));

        HU_ASSERT_EQ(hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome,
                                             &report),
                     HU_OK);
        HU_ASSERT_EQ(outcome, HU_GUARD_REWROTE);
        HU_ASSERT(strstr(out, markers[i]) == NULL);
        HU_ASSERT(report.stripped_harmony_tokens);
        alloc.free(alloc.ctx, out, out_len + 1);
    }
}

/* ── Degenerate repetition detection ──────────────────────────────────── */

static void guard_rejects_runaway_quote_loop(void) {
    /* 200 consecutive `" ` — the classic temperature/penalty failure. */
    char raw[800];
    raw[0] = '\0';
    for (int i = 0; i < 200; i++)
        strcat(raw, "\" ");

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);

    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(out == NULL);
    HU_ASSERT_EQ(out_len, 0u);
    HU_ASSERT(report.detected_degenerate_repetition);
}

static void guard_rejects_long_single_char_run(void) {
    /* 100 consecutive 'a' — should reject. */
    char raw[200];
    memset(raw, 'a', 100);
    raw[100] = '\0';

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);

    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_degenerate_repetition);
    HU_ASSERT(report.max_repetition_run >= 100u);
}

static void guard_passes_normal_text(void) {
    const char *raw = "Hey! Sounds good — see you at 7.";

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_REWROTE;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);

    HU_ASSERT_EQ(outcome, HU_GUARD_OK);
    /* OK path: out points at original. */
    HU_ASSERT(out == raw || (out != NULL && strcmp(out, raw) == 0));
    HU_ASSERT_EQ(out_len, strlen(raw));
}

static void guard_passes_normal_emoji_punctuation(void) {
    /* Lots of emoji + ! — must NOT trigger the degenerate guard, even
     * though it's repetitive in tone. */
    const char *raw = "Yessss!! 😂😂 omg same haha 🥹💕";

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_REWROTE;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);

    /* Could be OK or REWROTE depending on if any tokens are stripped, but
     * MUST NOT be REJECT for legitimate human-style text. */
    HU_ASSERT(outcome != HU_GUARD_REJECT);
    if (outcome == HU_GUARD_REWROTE && out != NULL) {
        alloc.free(alloc.ctx, out, out_len + 1);
    }
}

static void guard_passes_legitimate_repeated_word(void) {
    /* "very very very good" — 3-repeats of a short word should NOT trigger
     * (threshold is well above natural repetition). */
    const char *raw = "very very very good idea!";

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_REJECT;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);

    HU_ASSERT(outcome != HU_GUARD_REJECT);
    if (outcome == HU_GUARD_REWROTE && out != NULL) {
        alloc.free(alloc.ctx, out, out_len + 1);
    }
}

/* ── Edge cases ───────────────────────────────────────────────────────── */

static void guard_handles_empty_input(void) {
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_REWROTE;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    HU_ASSERT_EQ(hu_response_guard_check(&alloc, "", 0, &out, &out_len, &outcome, &report), HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_OK);
    HU_ASSERT_EQ(out_len, 0u);
}

static void guard_handles_null_args(void) {
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;

    HU_ASSERT_EQ(hu_response_guard_check(NULL, "x", 1, &out, &out_len, &outcome, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_response_guard_check(&alloc, NULL, 1, &out, &out_len, &outcome, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_response_guard_check(&alloc, "x", 1, NULL, &out_len, &outcome, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_response_guard_check(&alloc, "x", 1, &out, NULL, &outcome, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_response_guard_check(&alloc, "x", 1, &out, &out_len, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── Lower-level helper coverage ──────────────────────────────────────── */

static void helper_has_special_token_detects_harmony(void) {
    HU_ASSERT(hu_response_guard_has_special_token("hello <|channel|> world", 24));
    HU_ASSERT(hu_response_guard_has_special_token("<|message|>", 11));
    HU_ASSERT(hu_response_guard_has_special_token("a <think>x</think> b", 20));
    HU_ASSERT(!hu_response_guard_has_special_token("hey, see you at 7!", 18));
    HU_ASSERT(!hu_response_guard_has_special_token("a < b and c |> d", 16)); /* not a token */
}

static void helper_longest_char_run_counts(void) {
    HU_ASSERT_EQ(hu_response_guard_longest_char_run("aaabbbcccddddd", 14), 5u);
    HU_ASSERT_EQ(hu_response_guard_longest_char_run("abcdefg", 7), 1u);
    HU_ASSERT_EQ(hu_response_guard_longest_char_run("", 0), 0u);
}

static void helper_longest_token_run_counts(void) {
    HU_ASSERT(hu_response_guard_longest_token_run("\" \" \" \" \" \" \"", 13) >= 6u);
    HU_ASSERT(hu_response_guard_longest_token_run("the quick brown fox", 19) <= 1u);
}

static void guard_strips_gemma4_untagged_reasoning(void) {
    const char *raw =
        "\n*   User: \"Hey you!\"\n"
        "    *   Persona: Seth Douglas Ford (51, Chief Architect)\n"
        "    *   Context: Annie is teasing Seth.\n"
        "\n"
        "hey! what's up";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome;
    hu_guard_report_t report;
    hu_error_t err =
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REWROTE);
    HU_ASSERT(report.stripped_thinking_block);
    HU_ASSERT(out != NULL);
    HU_ASSERT(out_len > 0);
    HU_ASSERT(strncmp(out, "hey! what's up", 14) == 0);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void guard_rejects_gemma4_reasoning_only(void) {
    const char *raw =
        "\n*   User (Annie Ford, Seth's sister) is asking about Doug\n"
        "    *   Persona: Seth Douglas Ford\n"
        "    *   Options: 1) Ask more 2) Redirect\n";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome;
    hu_guard_report_t report;
    hu_error_t err =
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(out == NULL);
    HU_ASSERT_EQ(out_len, 0u);
}

static void guard_passes_normal_asterisk_text(void) {
    const char *raw = "I think we should *definitely* go for it!";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome;
    hu_error_t err =
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_OK);
    HU_ASSERT(out == raw);
}

/* ── Registration ─────────────────────────────────────────────────────── */

void run_response_guard_tests(void) {
    HU_TEST_SUITE("Response Guard");

    HU_RUN_TEST(guard_rejects_or_sanitizes_production_harmony_leak);
    HU_RUN_TEST(guard_strips_harmony_channel_marker);
    HU_RUN_TEST(guard_strips_unclosed_channel_token);
    HU_RUN_TEST(guard_strips_thinking_block);
    HU_RUN_TEST(guard_strips_capital_thought_block);
    HU_RUN_TEST(guard_strips_all_known_harmony_markers);
    HU_RUN_TEST(guard_rejects_runaway_quote_loop);
    HU_RUN_TEST(guard_rejects_long_single_char_run);
    HU_RUN_TEST(guard_passes_normal_text);
    HU_RUN_TEST(guard_passes_normal_emoji_punctuation);
    HU_RUN_TEST(guard_passes_legitimate_repeated_word);
    HU_RUN_TEST(guard_handles_empty_input);
    HU_RUN_TEST(guard_handles_null_args);
    HU_RUN_TEST(helper_has_special_token_detects_harmony);
    HU_RUN_TEST(helper_longest_char_run_counts);
    HU_RUN_TEST(helper_longest_token_run_counts);
    HU_RUN_TEST(guard_strips_gemma4_untagged_reasoning);
    HU_RUN_TEST(guard_rejects_gemma4_reasoning_only);
    HU_RUN_TEST(guard_passes_normal_asterisk_text);
}
