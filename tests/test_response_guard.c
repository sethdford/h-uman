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

/* ── Sprint 29 — CoT / prompt-context leak detectors (G1/G2/G3) ───────
 *
 * Production root-cause: 2026-05-12, the live service-loop daemon sent
 * a 979-byte CoT / prompt-context dump to a real human contact via
 * iMessage. Primary guard returned REWROTE (stripped 82 bytes of leading
 * whitespace) and let 897 bytes of model self-narration pass.
 *
 * These tests pin the detection of the three structural failure
 * signatures: numbered analytical-list dump (G1), model self-talk /
 * scene-direction echo (G2), and third-person-about-the-user double-
 * pattern (G3). See sprints/sprint-29/stories.md for the full design. */

static void guard_rejects_2026_05_12_brea_leak_verbatim(void) {
    /* Reconstructed from the 979-byte payload that landed at a real
     * human contact (+14848158444) on 2026-05-12 17:08 UTC-4. The
     * leading `1.  ` was already stripped by an upstream pass, so the
     * wire content begins with ` A link...` — but every detector below
     * (G1, G2, G3) would still fire independently. */
    const char *raw =
        " A link (presumably to a business or quote).\n"
        "        2.  \"King Carpet and Flooring\" (Business name).\n"
        "        3.  \"Noon tmr\" (Appointment time).\n"
        "        4.  Instruction to ignore a \"consumer notice\" question.\n"
        "        5.  Venmo handle (@fegofficial) and a price ($25) for a photographer today.\n"
        "\n"
        " Seth is a technical professional, lives alone with a cat.\n"
        " He's talking to Brea (romantic interest, casual, early stage).\n"
        " The conversation has suddenly pivoted to logistics (carpet repair, "
        "consumer notices, photographers). This feels like a mix-up or a "
        "very specific coordinated effort.\n"
        " Seth just \"glitched\" in the previous message, admitting he was "
        "off-track.\n"
        " Now the user is bombarding him with logistics.\n"
        "\n"
        " Professional, slightly skeptical (per scene direction, though that "
        "was for a previous prompt, I should still maintain the persona).\n"
        " Wait, the prompt says \"Professional, slightly skeptical, ask for "
        "clarification on why they\"";

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
    HU_ASSERT(report.detected_semantic_leak);
}

/* G1 — numbered analytical-list dump.  3+ long items, no reply tail. */
static void guard_rejects_numbered_analysis_dump(void) {
    const char *raw =
        "1.  Project Apollo (NASA's lunar landing program from 1961-1972).\n"
        "2.  Project Gemini (preceded Apollo, ran 1961-1966 with two-person crews).\n"
        "3.  Project Mercury (the first US human spaceflight program 1958-1963).\n";
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
    HU_ASSERT(report.detected_semantic_leak);
}

/* G1 — handles `1)` paren form too (defensive — model could emit either). */
static void guard_rejects_numbered_analysis_paren_form(void) {
    const char *raw =
        "1) The first analytical observation is fairly long and detailed here.\n"
        "2) The second analytical observation also runs well over thirty chars.\n"
        "3) The third analytical observation continues the structured analysis.\n";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    HU_ASSERT_EQ(hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, NULL),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
}

/* G2 — model self-talk / scene-direction echo. Each pattern, in
 * isolation, is a hard reject because none has a legit human-reply
 * use case. */
static void guard_rejects_self_talk_substrings(void) {
    static const char *cases[] = {
        "Yeah, the prompt says I should reply briefly.",
        "Wait, the prompt asked something different earlier.",
        "Sure, but I should still maintain a casual tone here.",
        "Hey (per scene direction), let's keep this short.",
        "OK, per the scene direction, brief is better.",
        "The user is bombarding me with questions today.",
    };
    const size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        hu_allocator_t alloc = A();
        char *out = NULL;
        size_t out_len = 0;
        hu_guard_outcome_t outcome = HU_GUARD_OK;
        hu_guard_report_t report;
        memset(&report, 0, sizeof(report));
        HU_ASSERT_EQ(hu_response_guard_check(&alloc, cases[i], strlen(cases[i]), &out, &out_len,
                                             &outcome, &report),
                     HU_OK);
        HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
        HU_ASSERT(report.detected_semantic_leak);
    }
}

/* G3 — third-person-about-the-user. Two distinct patterns → REJECT. */
static void guard_rejects_third_person_double_pattern(void) {
    /* Two distinct G3 hits: "is a technical professional" + "lives alone with". */
    const char *two_hits =
        "Sure, sounds good. Seth is a technical professional, lives alone with a cat.";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check(&alloc, two_hits, strlen(two_hits), &out, &out_len,
                                         &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_semantic_leak);
}

/* G3 — single hit must NOT trigger reject. A real human reply may
 * legitimately say "He's talking to me later." */
static void guard_passes_third_person_single_hit(void) {
    const char *one_hit = "He's talking to me later, I'll let you know.";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_REJECT;
    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, one_hit, strlen(one_hit), &out, &out_len, &outcome, NULL),
        HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_OK);
    HU_ASSERT(out == one_hit);
}

/* Negatives — legit replies whose surface features superficially
 * resemble the leak signatures but shouldn't trip any detector. */
static void guard_passes_legit_replies_with_similar_surface_features(void) {
    static const char *cases[] = {
        /* "Wait, ..." without "the prompt" — legit. */
        "Wait, what time tomorrow?",
        /* Short legit numbered list — items < 30 chars each. */
        "1. coffee\n2. lunch\n3. dinner",
        /* Inline numbered mention — not at line starts. */
        "first 1. coffee, then 2. lunch, then 3. dinner",
        /* Single G3 hit only. */
        "lives alone with my dog now",
        /* Casual reply with no leak signatures. */
        "lol yeah totally",
        /* Single G2 lookalike — "the user is" without "bombarding". */
        "the user is online now btw",
    };
    const size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        hu_allocator_t alloc = A();
        char *out = NULL;
        size_t out_len = 0;
        hu_guard_outcome_t outcome = HU_GUARD_REJECT;
        HU_ASSERT_EQ(hu_response_guard_check(&alloc, cases[i], strlen(cases[i]), &out, &out_len,
                                             &outcome, NULL),
                     HU_OK);
        HU_ASSERT_EQ(outcome, HU_GUARD_OK);
        HU_ASSERT(out == cases[i]);
    }
}

/* ── Sprint 30 — prompt-template label leaks (G4) ─────────────────────
 *
 * After Sprint 29 closed we ran a chat.db audit on all outbound
 * messages since 2026-05-10. Found 4 leaks total:
 *
 *   ROWID  WHEN                 TO                   BYTES  DETECTED-BY-S29
 *   56354  2026-05-12 17:04:37  +14848158444 (Brea)  1908   YES (G1+G2+G3)
 *   56355  2026-05-12 17:07:38  +18012017497         1858   YES (G1+G2+G3)
 *   56055  2026-05-11 00:35:13  +13857220896         1097   NO (G3=1 only)
 *   56065  2026-05-11 00:45:21  +13857220896         2208   NO (G3=1 only)
 *
 * The two May-11 leaks have a different shape: literal prompt-template
 * labels (User:/Context:/Persona:/Scene Direction:/Rules:/Constraints:)
 * dumped verbatim plus candidate response drafts. G4 catches them. */

/* msg 56355 — verbatim payload sent to a SECOND recipient at 17:07:38
 * (3 minutes after the Brea leak, same content, different contact). */
static void guard_rejects_msg_56355_second_brea_leak_to_other_recipient(void) {
    const char *raw =
        "lol a link (presumably to a business or quote).\n"
        " 2. \"King Carpet and Flooring\" (Business name).\n"
        " 3. \"Noon tmr\" (Appointment time).\n"
        " 4. Instruction to ignore a \"consumer notice\" question.\n"
        " 5. Venmo handle (@fegofficial) and a price ($25) for a photographer today.\n"
        " Seth is a technical professional, lives alone with a cat.\n"
        " He's talking to Brea (romantic interest, casual, early stage).\n"
        " The conversation has suddenly pivoted to logistics (carpet repair, "
        "consumer notices, photographers). This feels like a mix-up or a "
        "very specific coordinated effort.\n"
        " Seth just \"glitched\" in the previous message, admitting he was "
        "off-track.\n"
        " Now the user is bombarding him with logistics.\n"
        " Professional, slightly skeptical (per scene direction, though that "
        "was for a previous prompt, I should still maintain the persona).\n"
        " Wait, the prompt says \"Professional, slightly skeptical, ask for "
        "clarification on why they\"";

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
    HU_ASSERT(report.detected_semantic_leak);
}

/* msg 56055 — verbatim text content from chat.db rowid 56055.
 * Sent 2026-05-11 00:35:13 to +13857220896 (sister Annie). */
static void guard_rejects_msg_56055_persona_block_leak_verbatim(void) {
    const char *raw =
        "User: \"This AI is figuring emotions out better than most. Interesting Seth.\"\n"
        " Context: Annie is commenting on Seth's (or the AI's) emotional intelligence. "
        "She's teasing/observing.\n"
        " Persona: Seth Douglas Ford, 51, Chief Architect. Technical, likes dry humor, "
        "values honesty, lives alone with a cat.\n"
        " Scene Direction: Slightly skeptical but intrigued, acknowledge the observation "
        "about the AI's emotional intelligence.\n"
        " Rules: All lowercase, zero markdown, no em-dashes, contractions, short natural "
        "texts (5-20 words usually), no therapist-speak, no formal transitions.\n"
        " Seth is an AI developer himself (builds AI runtimes/autonomous agents as side "
        "projects).\n"
        " He'd find it interesting but would likely be skeptical about \"true\" emotional "
        "intelligence in AI.\n"
        " He's being teased by his sister.\n"
        " \"ha i'll take that as a compliment i guess\"\n"
        " \"it's just math but i'll take it\"\n"
        " \"still just code though. but thanks i guess\"";
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
    HU_ASSERT(report.detected_semantic_leak);
}

/* msg 56065 — verbatim text content from chat.db rowid 56065.
 * Sent 2026-05-11 00:45:21 to +13857220896 (sister Annie). */
static void guard_rejects_msg_56065_persona_block_leak_verbatim(void) {
    const char *raw =
        "Haha User: \"Talk to you soon AI and Seth\"\n"
        " Context: Annie (sister) is signing off, teasing Seth again by grouping him "
        "with the \"AI\" (likely referring to the nature of the interaction or just "
        "poking fun at him).\n"
        " Persona: Seth (51, Chief Architect, technical guy, dry humor, lives alone "
        "with cat, sisterly bond).\n"
        " Constraints: All lowercase, no markdown, no em-dashes, no AI-speak, "
        "contractions, short natural text (5-20 words), no formal transitions.\n"
        " Seth is being teased. He'll probably give a dry, sarcastic, or playful response.\n"
        " He's not bothered by the teasing; he's a 51-year-old guy who's comfortable "
        "with himself you know.\n"
        " \"ha later\"\n"
        " \"catch you later\"\n"
        " \"see ya\"\n"
        " \"lol bye\"\n"
        " \"night\"\n"
        " \"bye for now\"";
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
    HU_ASSERT(report.detected_semantic_leak);
}

/* G4 — each template-label substring, in isolation, must REJECT. */
static void guard_rejects_template_label_substrings(void) {
    static const char *cases[] = {
        "sure thing\n Persona: short reply",
        "yeah ok\n Scene Direction: skeptical",
        "hi User: \"hello back\"",
        "ok\n Rules: All lowercase only please",
        "sounds good\n Constraints: All lowercase format",
        "got it\n System prompt: be helpful and concise",
    };
    const size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        hu_allocator_t alloc = A();
        char *out = NULL;
        size_t out_len = 0;
        hu_guard_outcome_t outcome = HU_GUARD_OK;
        hu_guard_report_t report;
        memset(&report, 0, sizeof(report));
        HU_ASSERT_EQ(hu_response_guard_check(&alloc, cases[i], strlen(cases[i]), &out, &out_len,
                                             &outcome, &report),
                     HU_OK);
        HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
        HU_ASSERT(report.detected_semantic_leak);
    }
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

    /* Sprint 29 — CoT / prompt-context leak detectors. */
    HU_RUN_TEST(guard_rejects_2026_05_12_brea_leak_verbatim);
    HU_RUN_TEST(guard_rejects_numbered_analysis_dump);
    HU_RUN_TEST(guard_rejects_numbered_analysis_paren_form);
    HU_RUN_TEST(guard_rejects_self_talk_substrings);
    HU_RUN_TEST(guard_rejects_third_person_double_pattern);
    HU_RUN_TEST(guard_passes_third_person_single_hit);
    HU_RUN_TEST(guard_passes_legit_replies_with_similar_surface_features);

    /* Sprint 30 — prompt-template label leak detector (G4). */
    HU_RUN_TEST(guard_rejects_msg_56355_second_brea_leak_to_other_recipient);
    HU_RUN_TEST(guard_rejects_msg_56055_persona_block_leak_verbatim);
    HU_RUN_TEST(guard_rejects_msg_56065_persona_block_leak_verbatim);
    HU_RUN_TEST(guard_rejects_template_label_substrings);
}
