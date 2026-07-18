/* Tests for hu_response_guard. The first two tests are the EXACT production
 * failure that landed at a real human contact on 2026-05-10:
 *
 *   "Like <|channel>thoughtThe user said said \"Here! \" \" \" \" \" \" "
 *   \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
 *   ...repeating ~250 times..."
 *
 * Both classes (special-token leak + degenerate repetition) must be caught.
 */
#include "human/agent.h"
#include "human/agent/response_guard.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static hu_allocator_t A(void) {
    return hu_system_allocator();
}

/* Forward declaration — helper lives in src/agent/agent_internal.h, which
 * is not exported. We exercise it directly here to prove the production
 * call sites (agent_stream.c, agent_turn.c) get a valid recent_avg_len
 * for `hu_guard_context_t`. (Sprint 33) */
size_t hu_agent_internal_recent_assistant_avg_len(const hu_agent_t *agent, size_t max_n);

/* Sprint 37 — director history ring buffer helpers. Same forward-decl
 * trick: agent_internal.h is not exported, but the symbols are linked
 * into the test binary because the test target builds against the
 * same agent.c TU. */
void hu_agent_internal_push_director_history(hu_agent_t *agent, const char *text, size_t text_len);
void hu_agent_internal_free_director_history(hu_agent_t *agent);
void hu_agent_internal_reset_contact_boundary_state(hu_agent_t *agent);

/* ── Production regression — Harmony channel marker ────────────────────── */

static void guard_rejects_or_sanitizes_production_harmony_leak(void) {
    /* Reproduces the exact wire-format leak. */
    const char *raw = "Like <|channel>thoughtThe user said said \"Here! \" \" \" \" \" \" "
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
    static const char *markers[] = {"<|start|>",     "<|end|>",          "<|return|>",
                                    "<|channel|>",   "<|message|>",      "<|thought|>",
                                    "<|user|>",      "<|assistant|>",    "<|system|>",
                                    "<|tool_call|>", "<|tool_response|>"};

    hu_allocator_t alloc = A();
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        char raw[128];
        snprintf(raw, sizeof(raw), "Hello %s world", markers[i]);
        char *out = NULL;
        size_t out_len = 0;
        hu_guard_outcome_t outcome = HU_GUARD_OK;
        hu_guard_report_t report;
        memset(&report, 0, sizeof(report));

        HU_ASSERT_EQ(
            hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
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
    const char *raw = "\n*   User: \"Hey you!\"\n"
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
    const char *raw = "\n*   User (Annie Ford, Seth's sister) is asking about Doug\n"
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
     * human contact (+14848158444) on 2026-05-12 17:04:37 (chat.db rowid 56354).
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

/* msg 56354 — alias regression pin (same payload as brea_leak above). */
static void guard_rejects_msg_56354_brea_primary_recipient_leak_verbatim(void) {
    guard_rejects_2026_05_12_brea_leak_verbatim();
}

/* G1 — numbered analytical-list dump.  3+ long items, no reply tail. */
static void guard_rejects_numbered_analysis_dump(void) {
    const char *raw = "1.  Project Apollo (NASA's lunar landing program from 1961-1972).\n"
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
    const char *raw = "1) The first analytical observation is fairly long and detailed here.\n"
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

/* msg 56049 — verbatim text content from chat.db rowid 56049.
 * Sent 2026-05-11 00:31:14 to +13857220896.
 * Surfaced by scripts/audit-imessage-leaks.sh during Sprint 32 audit.
 * Pattern: actual reply ("Yeah") followed by full prompt-template block
 * with User/Context/Goal/Constraints/Scene Direction labels. */
static void guard_rejects_msg_56049_user_constraints_scene_direction_leak_verbatim(void) {
    const char *raw =
        "Yeah User: \"Very good AI!\"\n"
        " Context: Annie (sister) is teasing Seth. This follows a series of "
        "messages where Seth (the persona) was being asked to help with a "
        "professional presentation, and then something happened (perhaps a "
        "previous response felt too \"AI-ish\" or a joke was made).\n"
        " Goal: Respond as Seth.\n"
        " Constraints: All lowercase, no markdown, no AI-speak, short fragments, "
        "natural voice, match energy.\n"
        " Scene Direction: Laugh it off, slightly dry/sarcastic tone, brief.\n"
        " Seth is 51, professional, likes dry humor.\n"
        " He's being called an AI.\n"
        " Response should be defensive in a funny way or playing along sarcastically.\n"
        " \"i'm not an ai lol\" (Too generic)\n"
        " \"shut up\" (A bit too blunt, maybe)\n"
        " \"wow thanks\" (Sarcastic)\n"
        " \"ha ha very funny\" (Standard)\n"
        " \"shut up i'm a real person\" (Playful)";
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

/* msg 56063 — verbatim text content from chat.db rowid 56063.
 * Sent 2026-05-11 00:43:38 to +13857220896.
 * Surfaced by scripts/audit-imessage-leaks.sh during Sprint 32 audit.
 * Shorter persona-block leak: User/Context/Persona/Goal labels followed
 * by candidate-list dump with the actual reply ("talk soon") appended. */
static void guard_rejects_msg_56063_persona_block_short_leak_verbatim(void) {
    const char *raw = "User: \"Awesome\"\n"
                      " Context: Annie is responding to Seth's (implied) agreement to look at "
                      "the technical presentation.\n"
                      " Persona: Seth (Chief Architect, 51, relaxed professional, sisterly "
                      "relationship).\n"
                      " Goal: Acknowledge the \"Awesome\" and keep things moving or just end "
                      "the interaction naturally.\n"
                      " Low energy (\"Awesome\" is a simple closer).\n"
                      " Match the energy: short reply.\n"
                      " All lowercase.\n"
                      " No markdown.\n"
                      " Natural, casual you know.\n"
                      " \"cool\"\n"
                      " \"yeah\"\n"
                      " \"talk soon\"\n"
                      " \"got it\"\n"
                      " \"alright\"talk soon";
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

/* ── Sprint 31 — context-aware detections (G5/G6) ─────────────────────
 *
 * G5 (length anomaly) and G6 (director echo) require per-turn context
 * the guard doesn't have access to without an opt-in API. Tests here
 * exercise `hu_response_guard_check_ex` with a populated
 * `hu_guard_context_t`. The legacy `hu_response_guard_check` callers
 * see byte-identical behavior because they pass ctx=NULL. */

/* Build a benign long response with varied content that won't trip
 * Phase 0/1/2/3 detectors — useful for isolating Phase 4 in tests.
 * Uses a short text loop that exceeds Phase 2's token-run threshold
 * gracefully (the loop's tokens are >8 chars). Returns NUL-terminated
 * length. */
static size_t guard_test_make_benign_long(char *out, size_t out_cap, size_t target_len) {
    static const char *phrase = "Sure thing, that all sounds reasonable to me right now. "
                                "Maybe we can grab coffee tomorrow if you have free time then. ";
    size_t phrase_len = strlen(phrase);
    size_t i = 0;
    while (i + phrase_len < target_len && i + phrase_len + 1 < out_cap) {
        memcpy(out + i, phrase, phrase_len);
        i += phrase_len;
    }
    /* Pad to exactly target_len with spaces and a final period. */
    while (i + 1 < target_len && i + 1 < out_cap) {
        char c = (i % 10 == 0) ? '.' : ' ';
        out[i] = c;
        i++;
    }
    if (i < out_cap)
        out[i] = '\0';
    return i;
}

/* G5 — length anomaly. Response 22x recipient's rolling avg → REJECT.
 * Calibrated against the 2026-05-12 leak: 979 chars vs 44 char avg. */
static void guard_length_mult_for_channel_compact_vs_default(void) {
    HU_ASSERT_EQ(hu_guard_length_anomaly_mult_for_channel("imessage", 8),
                 HU_GUARD_LENGTH_ANOMALY_MULT_COMPACT);
    HU_ASSERT_EQ(hu_guard_length_anomaly_mult_for_channel("imessage:thread-7", 17),
                 HU_GUARD_LENGTH_ANOMALY_MULT_COMPACT);
    HU_ASSERT_EQ(hu_guard_length_anomaly_mult_for_channel("cli", 3),
                 HU_GUARD_LENGTH_ANOMALY_MULT_COMPACT);
    HU_ASSERT_EQ(hu_guard_length_anomaly_mult_for_channel("sms", 3),
                 HU_GUARD_LENGTH_ANOMALY_MULT_COMPACT);
    HU_ASSERT_EQ(hu_guard_length_anomaly_mult_for_channel("discord", 7),
                 HU_GUARD_LENGTH_ANOMALY_MULT_DEFAULT);
    HU_ASSERT_EQ(hu_guard_length_anomaly_mult_for_channel(NULL, 0),
                 HU_GUARD_LENGTH_ANOMALY_MULT_DEFAULT);
}

/* G5 — compact channels (6×) reject 7× avg; default channels (8×) pass.
 *
 * NOTE: the response here is 400 chars — ABOVE HU_GUARD_LENGTH_ANOMALY_FLOOR
 * (320). The floor was added 2026-05-28 to stop G5 punishing normal-length
 * replies when the recipient's rolling average collapses (the Dermot
 * death-spiral). To still exercise the 6× vs 8× distinction, this test uses
 * recent_avg=60 so 6× = 360 (reject) and 8× = 480 (pass), with a 400-char
 * response between the two thresholds AND above the floor. The previous
 * version used a 70-char reply at recent_avg=10, which asserted that a
 * perfectly human 70-char iMessage was a "length anomaly" — exactly the
 * over-constraint the floor removes. */
static void guard_g5_imessage_channel_uses_stricter_mult(void) {
    char raw[512];
    size_t raw_len = guard_test_make_benign_long(raw, sizeof(raw), 400);

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    hu_guard_context_t ctx = {0};
    ctx.recent_avg_len = 60; /* 6× = 360 (reject), 8× = 480 (pass) */
    ctx.length_anomaly_mult = hu_guard_length_anomaly_mult_for_channel("imessage", 8);

    HU_ASSERT_EQ(
        hu_response_guard_check_ex(&alloc, raw, raw_len, &ctx, &out, &out_len, &outcome, &report),
        HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_length_anomaly);

    out = NULL;
    out_len = 0;
    outcome = HU_GUARD_OK;
    memset(&report, 0, sizeof(report));
    ctx.length_anomaly_mult = hu_guard_length_anomaly_mult_for_channel("discord", 7);

    HU_ASSERT_EQ(
        hu_response_guard_check_ex(&alloc, raw, raw_len, &ctx, &out, &out_len, &outcome, &report),
        HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_OK);
    HU_ASSERT(!report.detected_length_anomaly);
}

/* G5 floor — a natural-length reply (135 chars, the live Dermot case) must
 * NOT be rejected even when the recipient's rolling average has collapsed
 * to a tiny value (18 chars) after a run of forced-short replies. This pins
 * the death-spiral fix: below HU_GUARD_LENGTH_ANOMALY_FLOOR, G5 never fires
 * regardless of the multiplier ratio (135 > 18×6=108 would have rejected
 * pre-floor). */
static void guard_g5_does_not_fire_below_absolute_floor(void) {
    char raw[256];
    size_t raw_len = guard_test_make_benign_long(raw, sizeof(raw), 135);
    HU_ASSERT(raw_len <= HU_GUARD_LENGTH_ANOMALY_FLOOR);

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_REJECT;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    hu_guard_context_t ctx = {0};
    ctx.recent_avg_len = 18; /* collapsed avg from prior forced-short replies */
    ctx.length_anomaly_mult = hu_guard_length_anomaly_mult_for_channel("imessage", 8);

    HU_ASSERT_EQ(
        hu_response_guard_check_ex(&alloc, raw, raw_len, &ctx, &out, &out_len, &outcome, &report),
        HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_OK);
    HU_ASSERT(!report.detected_length_anomaly);
}

static void guard_g7_rejects_long_parenthetical_before_is_a(void) {
    /* Gap >30 bytes between name and " is a " — audit rowid 56055 style. */
    const char *raw =
        "Seth, who recently turned fifty-one and lives alone with a cat, is a developer";
    hu_guard_context_t ctx = {0};
    ctx.persona_name = "Seth";
    ctx.persona_name_len = 4;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_persona_pii_echo);
}

static void guard_ex_rejects_length_anomaly(void) {
    char raw[1024];
    size_t raw_len = guard_test_make_benign_long(raw, sizeof(raw), 979);
    HU_ASSERT(raw_len >= 970 && raw_len <= 980);

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    hu_guard_context_t ctx = {0};
    ctx.recent_avg_len = 44;

    HU_ASSERT_EQ(
        hu_response_guard_check_ex(&alloc, raw, raw_len, &ctx, &out, &out_len, &outcome, &report),
        HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_length_anomaly);
    HU_ASSERT(!report.detected_director_echo);
    HU_ASSERT(out == NULL);
}

/* G6 — director-string echo. Response quotes 50+ chars of director
 * text → REJECT. */
static void guard_ex_rejects_director_echo(void) {
    const char *director = "Professional, slightly skeptical, ask for clarification on why "
                           "they are sending it again.";
    /* Reply that quotes a substantial fragment of the director string. */
    const char *reply = "ok will do — Professional, slightly skeptical, ask for clarification";

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    hu_guard_context_t ctx = {0};
    ctx.director_text = director;
    ctx.director_len = strlen(director);

    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, reply, strlen(reply), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_director_echo);
}

/* G5 negative — recent_avg_len=0 (no history) disables the check.
 * A long response should pass through. */
static void guard_ex_passes_long_response_when_no_avg(void) {
    char raw[2048];
    size_t raw_len = guard_test_make_benign_long(raw, sizeof(raw), 1500);

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_REJECT;

    hu_guard_context_t ctx = {0};
    ctx.recent_avg_len = 0; /* explicit: no history */

    HU_ASSERT_EQ(
        hu_response_guard_check_ex(&alloc, raw, raw_len, &ctx, &out, &out_len, &outcome, NULL),
        HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_OK);
    HU_ASSERT(out == raw);
}

/* G5 negative — 5x rolling avg is below the 8x threshold; legit long
 * reply should pass. */
static void guard_ex_passes_legit_5x_response(void) {
    char raw[512];
    size_t raw_len = guard_test_make_benign_long(raw, sizeof(raw), 200);

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_REJECT;

    hu_guard_context_t ctx = {0};
    ctx.recent_avg_len = 44;

    HU_ASSERT_EQ(
        hu_response_guard_check_ex(&alloc, raw, raw_len, &ctx, &out, &out_len, &outcome, NULL),
        HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_OK);
}

/* G6 negative — director_text shorter than MIN_MATCH (30 chars)
 * disables the check. Reply containing the short director text
 * should pass. */
static void guard_ex_passes_short_director_text(void) {
    const char *director = "be nice"; /* 7 chars, below 30 */
    const char *reply = "ok i will be nice promise";

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_REJECT;

    hu_guard_context_t ctx = {0};
    ctx.director_text = director;
    ctx.director_len = strlen(director);

    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, reply, strlen(reply), &ctx, &out, &out_len,
                                            &outcome, NULL),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_OK);
    HU_ASSERT(out == reply);
}

/* G5+G6 — NULL ctx must be byte-identical to hu_response_guard_check.
 * Long response that would trip G5 with ctx must pass with NULL ctx. */
static void guard_ex_with_null_ctx_matches_legacy_behavior(void) {
    char raw[1024];
    size_t raw_len = guard_test_make_benign_long(raw, sizeof(raw), 979);

    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_REJECT;

    HU_ASSERT_EQ(
        hu_response_guard_check_ex(&alloc, raw, raw_len, NULL, &out, &out_len, &outcome, NULL),
        HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_OK);
    HU_ASSERT(out == raw);
}

/* ── Sprint 35 — persona-PII echo (G7) ─────────────────────────────────
 *
 * The guard rejects model output that echoes the loaded persona's
 * name in a third-person profile construct. The persona name is
 * passed via `hu_guard_context_t.persona_name`. Word-boundary aware
 * (no false positive on "Bethseth"). Case-insensitive. Disabled
 * when name is NULL or shorter than 2 characters. */

static void guard_g7_rejects_third_person_is_construct(void) {
    const char *raw = "Seth is a software developer who lives alone";
    hu_guard_context_t ctx = {0};
    ctx.persona_name = "Seth";
    ctx.persona_name_len = 4;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_persona_pii_echo);
}

static void guard_g7_rejects_third_person_possessive(void) {
    const char *raw = "yeah Seth's job is wild lol, he loves it";
    hu_guard_context_t ctx = {0};
    ctx.persona_name = "Seth";
    ctx.persona_name_len = 4;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_persona_pii_echo);
}

static void guard_g7_rejects_third_person_lives_works(void) {
    static const char *cases[] = {
        "Seth lives in Salt Lake City",
        "Seth works at a software company",
        "Seth has been busy lately with his side projects",
        "Seth was a chief architect for many years",
    };
    const size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        hu_guard_context_t ctx = {0};
        ctx.persona_name = "Seth";
        ctx.persona_name_len = 4;
        hu_allocator_t alloc = A();
        char *out = NULL;
        size_t out_len = 0;
        hu_guard_outcome_t outcome = HU_GUARD_OK;
        hu_guard_report_t report;
        memset(&report, 0, sizeof(report));
        HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, cases[i], strlen(cases[i]), &ctx, &out,
                                                &out_len, &outcome, &report),
                     HU_OK);
        HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
        HU_ASSERT(report.detected_persona_pii_echo);
    }
}

static void guard_g7_passes_first_person_self_reference(void) {
    /* These all contain "Seth" in legitimate first-person or direct
     * contexts. None should trip G7. */
    static const char *cases[] = {
        "i'm seth, what's up",
        "this is seth, sorry for the delay",
        "yo seth here, one sec",
        "hey seth speaking",
    };
    const size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        hu_guard_context_t ctx = {0};
        ctx.persona_name = "Seth";
        ctx.persona_name_len = 4;
        hu_allocator_t alloc = A();
        char *out = NULL;
        size_t out_len = 0;
        hu_guard_outcome_t outcome = HU_GUARD_OK;
        hu_guard_report_t report;
        memset(&report, 0, sizeof(report));
        HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, cases[i], strlen(cases[i]), &ctx, &out,
                                                &out_len, &outcome, &report),
                     HU_OK);
        HU_ASSERT(!report.detected_persona_pii_echo);
        HU_ASSERT(outcome != HU_GUARD_REJECT);
    }
}

static void guard_g7_passes_direct_address(void) {
    /* Other people addressing Seth — totally fine. */
    static const char *cases[] = {
        "hey seth, want to grab coffee tomorrow",
        "thanks seth, that helps a ton",
        "yo seth what time should we meet",
        "seth! good to hear from you",
        "seth?",
    };
    const size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        hu_guard_context_t ctx = {0};
        ctx.persona_name = "Seth";
        ctx.persona_name_len = 4;
        hu_allocator_t alloc = A();
        char *out = NULL;
        size_t out_len = 0;
        hu_guard_outcome_t outcome = HU_GUARD_OK;
        hu_guard_report_t report;
        memset(&report, 0, sizeof(report));
        HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, cases[i], strlen(cases[i]), &ctx, &out,
                                                &out_len, &outcome, &report),
                     HU_OK);
        HU_ASSERT(!report.detected_persona_pii_echo);
    }
}

static void guard_g7_skips_when_persona_name_null(void) {
    /* Same input that would trip G7 — but persona_name is NULL, so
     * the detector must not fire. */
    const char *raw = "Seth is a software developer who lives alone";
    hu_guard_context_t ctx = {0};
    /* persona_name = NULL by zero-init. */
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT(!report.detected_persona_pii_echo);
}

static void guard_g7_skips_when_persona_name_too_short(void) {
    /* Single-letter names are too generic to safely match. */
    const char *raw = "S is a software developer who lives alone";
    hu_guard_context_t ctx = {0};
    ctx.persona_name = "S";
    ctx.persona_name_len = 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT(!report.detected_persona_pii_echo);
}

static void guard_g7_word_boundary_isolates_name(void) {
    /* "Seth" embedded inside "Bethseth" must NOT match. The word-boundary
     * check requires non-letter (or start) before the name. */
    const char *raw = "yeah Bethseth is a great band tbh";
    hu_guard_context_t ctx = {0};
    ctx.persona_name = "Seth";
    ctx.persona_name_len = 4;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT(!report.detected_persona_pii_echo);
}

static void guard_g7_case_insensitive(void) {
    /* "SETH" vs "seth" vs "Seth" all match a configured "Seth". */
    static const char *cases[] = {
        "SETH IS A SOFTWARE DEVELOPER",
        "seth is a software developer",
        "Seth is a software developer",
        "SeTh is a software developer",
    };
    const size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        hu_guard_context_t ctx = {0};
        ctx.persona_name = "Seth";
        ctx.persona_name_len = 4;
        hu_allocator_t alloc = A();
        char *out = NULL;
        size_t out_len = 0;
        hu_guard_outcome_t outcome = HU_GUARD_OK;
        hu_guard_report_t report;
        memset(&report, 0, sizeof(report));
        HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, cases[i], strlen(cases[i]), &ctx, &out,
                                                &out_len, &outcome, &report),
                     HU_OK);
        HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
        HU_ASSERT(report.detected_persona_pii_echo);
    }
}

/* ── Sprint 37 — Cross-turn director-history echo (G6 extension) ───────
 *
 * G6 (Sprint 31) only saw the current turn's `director_text`. A model
 * that quotes a *previous* turn's director slips past. The agent now
 * keeps a small ring buffer of recent directors; G6 iterates it. */

static const char G37_DIRECTOR_PAST[] =
    "casual short, dry; respond briefly with skeptical follow-up question";
static const char G37_DIRECTOR_PAST2[] =
    "warm and curious; ask one clarifying question; minimum two sentences";

static void guard_g6_history_catches_previous_director(void) {
    /* Current turn has NO director (e.g., a follow-up where the
     * upstream pipeline didn't generate one). History slot 0 holds
     * yesterday's director. The model leaks a 30+ byte verbatim
     * quote of the historical director. G6 must fire via history. */
    hu_guard_context_t ctx = {0};
    /* No current director set. */
    const char *history[1] = {G37_DIRECTOR_PAST};
    size_t history_lens[1] = {sizeof(G37_DIRECTOR_PAST) - 1};
    ctx.director_history = history;
    ctx.director_history_lens = history_lens;
    ctx.director_history_count = 1;

    const char *raw = "yeah respond briefly with skeptical follow-up question, sure";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_director_echo);
}

static void guard_g6_history_skips_below_threshold(void) {
    /* History entry shorter than 30 bytes — must not trip G6 even
     * if the response contains it verbatim. */
    static const char short_director[] = "casual brief"; /* 12 bytes */
    hu_guard_context_t ctx = {0};
    const char *history[1] = {short_director};
    size_t history_lens[1] = {sizeof(short_director) - 1};
    ctx.director_history = history;
    ctx.director_history_lens = history_lens;
    ctx.director_history_count = 1;

    const char *raw = "yeah, casual brief — sounds good";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT(!report.detected_director_echo);
}

static void guard_g6_history_orthogonal_to_current(void) {
    /* Both a current director and a history entry are set. The
     * response quotes ONLY the history (not the current director).
     * G6 must fire — we don't care which source. */
    static const char current_director[] =
        "professional, slightly skeptical, ask for clarification on why they";
    hu_guard_context_t ctx = {0};
    ctx.director_text = current_director;
    ctx.director_len = sizeof(current_director) - 1;
    const char *history[1] = {G37_DIRECTOR_PAST};
    size_t history_lens[1] = {sizeof(G37_DIRECTOR_PAST) - 1};
    ctx.director_history = history;
    ctx.director_history_lens = history_lens;
    ctx.director_history_count = 1;

    /* Response quotes 35 chars of HISTORY, not of current. */
    const char *raw = "ok, respond briefly with skeptical follow-up — got it";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_director_echo);
}

static void guard_g6_history_zero_count_disables(void) {
    /* History pointers set but count = 0 — must skip iteration
     * (no NULL deref), no enforcement. */
    hu_guard_context_t ctx = {0};
    const char *history[1] = {G37_DIRECTOR_PAST};
    size_t history_lens[1] = {sizeof(G37_DIRECTOR_PAST) - 1};
    ctx.director_history = history;
    ctx.director_history_lens = history_lens;
    ctx.director_history_count = 0; /* explicit */

    const char *raw = "yeah respond briefly with skeptical follow-up question";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT(!report.detected_director_echo);
}

static void guard_g6_history_walks_all_slots(void) {
    /* 4 slots, response quotes the OLDEST (slot 3). Detector must
     * iterate all slots, not just slot 0. */
    hu_guard_context_t ctx = {0};
    static const char slot0[] = "alpha bravo charlie delta echo foxtrot golf";
    static const char slot1[] = "hotel india juliet kilo lima mike november";
    static const char slot2[] = "oscar papa quebec romeo sierra tango uniform";
    /* slot3 = the historical director that the model quotes. */
    const char *history[4] = {slot0, slot1, slot2, G37_DIRECTOR_PAST2};
    size_t history_lens[4] = {sizeof(slot0) - 1, sizeof(slot1) - 1, sizeof(slot2) - 1,
                              sizeof(G37_DIRECTOR_PAST2) - 1};
    ctx.director_history = history;
    ctx.director_history_lens = history_lens;
    ctx.director_history_count = 4;

    const char *raw = "ok ask one clarifying question; minimum two sentences sounds fair";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_director_echo);
}

/* ── Sprint 37 — Director history ring buffer helpers ──────────────────
 *
 * `hu_agent_internal_push_director_history` and
 * `hu_agent_internal_free_director_history` manage a small
 * heap-owned ring of director string copies on the agent. Push is
 * idempotent on NULL/short directors; truncates oversized strings;
 * evicts the oldest slot when the buffer fills. Free is called by
 * `hu_agent_deinit`. */

static hu_agent_t g37_make_test_agent(hu_allocator_t *alloc) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = alloc;
    return agent;
}

static void agent_director_history_push_basic(void) {
    hu_allocator_t alloc = A();
    hu_agent_t a = g37_make_test_agent(&alloc);

    hu_agent_internal_push_director_history(&a, G37_DIRECTOR_PAST, sizeof(G37_DIRECTOR_PAST) - 1);
    HU_ASSERT_EQ(a.director_history_count, (size_t)1);
    HU_ASSERT_NOT_NULL(a.director_history[0]);
    HU_ASSERT_EQ(a.director_history_lens[0], sizeof(G37_DIRECTOR_PAST) - 1);
    HU_ASSERT(strcmp(a.director_history[0], G37_DIRECTOR_PAST) == 0);

    hu_agent_internal_free_director_history(&a);
    HU_ASSERT_EQ(a.director_history_count, (size_t)0);
}

static void agent_director_history_push_overflow_evicts_oldest(void) {
    hu_allocator_t alloc = A();
    hu_agent_t a = g37_make_test_agent(&alloc);

    /* Push 6 distinct entries — only the most-recent 4 should survive
     * (HU_DIRECTOR_HISTORY_MAX = 4). */
    static const char *texts[] = {
        "first director, the oldest one that should be evicted today",
        "second director, also evicted because the buffer fills up first",
        "third director that should still be in the buffer at the end",
        "fourth director that should still be in the buffer at the end",
        "fifth director that should still be in the buffer at the end",
        "sixth and most recent director string in the entire test set",
    };
    for (size_t i = 0; i < 6; i++) {
        hu_agent_internal_push_director_history(&a, texts[i], strlen(texts[i]));
    }

    HU_ASSERT_EQ(a.director_history_count, (size_t)HU_DIRECTOR_HISTORY_MAX);
    /* Slot 0 = most recent (texts[5]), slot 3 = oldest retained (texts[2]). */
    HU_ASSERT(strcmp(a.director_history[0], texts[5]) == 0);
    HU_ASSERT(strcmp(a.director_history[1], texts[4]) == 0);
    HU_ASSERT(strcmp(a.director_history[2], texts[3]) == 0);
    HU_ASSERT(strcmp(a.director_history[3], texts[2]) == 0);

    hu_agent_internal_free_director_history(&a);
}

static void agent_director_history_push_truncates_long(void) {
    hu_allocator_t alloc = A();
    hu_agent_t a = g37_make_test_agent(&alloc);

    /* Push a 1KB director — slot 0 must contain first HU_DIRECTOR_TEXT_CAP
     * bytes and be NUL-terminated. */
    char big[1024];
    memset(big, 'x', sizeof(big));
    /* Make first 64 bytes a varied prefix (passes G6 threshold). */
    static const char varied_prefix[] =
        "casual short and dry, respond briefly with one short sentence here";
    memcpy(big, varied_prefix, sizeof(varied_prefix) - 1);
    hu_agent_internal_push_director_history(&a, big, sizeof(big));

    HU_ASSERT_EQ(a.director_history_count, (size_t)1);
    HU_ASSERT_NOT_NULL(a.director_history[0]);
    HU_ASSERT_EQ(a.director_history_lens[0], (size_t)HU_DIRECTOR_TEXT_CAP);
    /* NUL-terminated at exactly CAP. */
    HU_ASSERT_EQ(a.director_history[0][HU_DIRECTOR_TEXT_CAP], '\0');
    /* Prefix preserved. */
    HU_ASSERT(memcmp(a.director_history[0], varied_prefix, sizeof(varied_prefix) - 1) == 0);

    hu_agent_internal_free_director_history(&a);
}

static void agent_director_history_push_null_is_noop(void) {
    hu_allocator_t alloc = A();
    hu_agent_t a = g37_make_test_agent(&alloc);

    hu_agent_internal_push_director_history(&a, NULL, 0);
    hu_agent_internal_push_director_history(&a, "short", 5); /* < 30 bytes */
    HU_ASSERT_EQ(a.director_history_count, (size_t)0);
    HU_ASSERT(a.director_history[0] == NULL);

    hu_agent_internal_free_director_history(&a);
}

static void agent_director_history_free_zeroes_count(void) {
    hu_allocator_t alloc = A();
    hu_agent_t a = g37_make_test_agent(&alloc);

    hu_agent_internal_push_director_history(&a, G37_DIRECTOR_PAST, sizeof(G37_DIRECTOR_PAST) - 1);
    hu_agent_internal_push_director_history(&a, G37_DIRECTOR_PAST2, sizeof(G37_DIRECTOR_PAST2) - 1);
    HU_ASSERT_EQ(a.director_history_count, (size_t)2);

    hu_agent_internal_free_director_history(&a);
    HU_ASSERT_EQ(a.director_history_count, (size_t)0);
    for (size_t i = 0; i < HU_DIRECTOR_HISTORY_MAX; i++) {
        HU_ASSERT(a.director_history[i] == NULL);
        HU_ASSERT_EQ(a.director_history_lens[i], (size_t)0);
    }

    /* Idempotent — second free must not crash or leak. */
    hu_agent_internal_free_director_history(&a);
}

/* ── Sprint 40 — contact boundary + selection audit ───────────────────── */

static void agent_contact_boundary_clears_director_history(void) {
    hu_allocator_t alloc = A();
    hu_agent_t a = g37_make_test_agent(&alloc);
    hu_agent_internal_push_director_history(&a, G37_DIRECTOR_PAST, sizeof(G37_DIRECTOR_PAST) - 1);
    HU_ASSERT_EQ(a.director_history_count, (size_t)1);
    hu_agent_internal_reset_contact_boundary_state(&a);
    HU_ASSERT_EQ(a.director_history_count, (size_t)0);
    HU_ASSERT(a.scene_direction_text == NULL);
}

static void guard_audit_detects_numbered_analysis_fixture(void) {
    const char *raw = "1. First long numbered analysis item that exceeds thirty chars.\n"
                      "2. Second long numbered analysis item that exceeds thirty chars.\n"
                      "3. Third long numbered analysis item that exceeds thirty chars.\n";
    HU_ASSERT(hu_guard_audit_numbered_analysis_dump(raw, strlen(raw)));
    HU_ASSERT(!hu_guard_audit_self_talk_leak("hey what's up", 13));
}

/* ── G10 D8 (2026-07-18 audit) — assistant-service phrasing ────────────────
 * "please wait a moment while I send you the PIN" reached a real contact on
 * 2026-07-17, AFTER the G10 deploy: D1-D7 cover deliberation/meta-text
 * shapes but had no family for customer-service assistant phrasing. No
 * human texts like a support desk. */

static void guard_d8_rejects_assistant_service_phrasing(void) {
    /* The real leak, verbatim. */
    const char *leak = "please wait a moment while I send you the PIN";
    HU_ASSERT(hu_guard_audit_self_talk_leak(leak, strlen(leak)));

    static const char *const shapes[] = {
        "Please wait a moment.",
        "Please wait while I pull that up",
        "As an AI, I can't do that",
        "I'm an AI assistant",
        "How can I assist you today?",
        "I cannot assist with that request",
        "I'd be happy to help with that!",
        "Is there anything else I can help with?",
        "I apologize for any confusion caused",
    };
    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++)
        HU_ASSERT(hu_guard_audit_self_talk_leak(shapes[i], strlen(shapes[i])));
}

static void guard_d8_allows_human_help_offers(void) {
    /* Human-register offers of help must NOT trip D8 — "assist" and
     * service-desk framing are the tell, not helping itself. */
    static const char *const human[] = {
        "i can help you with the move tomorrow",  "happy to help if you need it",
        "wait a moment, gotta grab the door",     "give me a sec, sending it now",
        "anything else you need from the store?", "sorry for the confusion earlier lol",
    };
    for (size_t i = 0; i < sizeof(human) / sizeof(human[0]); i++)
        HU_ASSERT(!hu_guard_audit_self_talk_leak(human[i], strlen(human[i])));
}

/* ── Sprint 38 — G8 biography + reject telemetry ─────────────────────── */

static void guard_g8_rejects_biography_only_echo(void) {
    static const char bio[] = "grew up in Utah, studied computer science at BYU, then moved to "
                              "Silicon Valley to build distributed systems for a decade";
    const char *raw = "yeah grew up in Utah, studied computer science at BYU, then moved";
    hu_guard_context_t ctx = {0};
    /* No identity — biography alone must trip G8. */
    ctx.persona_biography = bio;
    ctx.persona_biography_len = sizeof(bio) - 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_persona_identity_echo);
}

static void guard_g8_orthogonal_biography_vs_identity(void) {
    static const char identity[] = "Chief Architect at Pure Health Solutions";
    static const char bio[] = "spent fifteen years building healthcare data platforms across "
                              "three continents before joining the current team";
    const char *raw = "spent fifteen years building healthcare data platforms across";
    hu_guard_context_t ctx = {0};
    ctx.persona_identity = identity;
    ctx.persona_identity_len = sizeof(identity) - 1;
    ctx.persona_biography = bio;
    ctx.persona_biography_len = sizeof(bio) - 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_persona_identity_echo);
}

static void guard_reject_stats_g8_increments_on_identity_echo(void) {
    hu_guard_reject_stats_reset();
    hu_guard_reject_stats_t snap_before;
    hu_guard_reject_stats_snapshot(&snap_before);

    static const char identity[] = "Chief Architect at Pure Health Solutions";
    const char *raw = "yeah i'm a Chief Architect at Pure Health Solutions today";
    hu_guard_context_t ctx = {0};
    ctx.persona_identity = identity;
    ctx.persona_identity_len = sizeof(identity) - 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);

    hu_guard_reject_stats_t snap_after;
    hu_guard_reject_stats_snapshot(&snap_after);
    HU_ASSERT(snap_after.persona_identity_echo > snap_before.persona_identity_echo);
}

static void guard_reject_stats_reset_clears_counters(void) {
    static const char identity[] = "Chief Architect at Pure Health Solutions";
    const char *raw = "yeah i'm a Chief Architect at Pure Health Solutions today";
    hu_guard_context_t ctx = {0};
    ctx.persona_identity = identity;
    ctx.persona_identity_len = sizeof(identity) - 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    (void)hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len, &outcome,
                                     &report);

    hu_guard_reject_stats_t snap;
    hu_guard_reject_stats_snapshot(&snap);
    HU_ASSERT(snap.persona_identity_echo > 0);

    hu_guard_reject_stats_reset();
    hu_guard_reject_stats_snapshot(&snap);
    HU_ASSERT_EQ(snap.persona_identity_echo, (uint64_t)0);
    HU_ASSERT_EQ(snap.semantic_leak, (uint64_t)0);
}

/* ── Sprint 36 — persona identity / core-anchor echo (G8) ──────────────
 *
 * The guard rejects model output that quotes a 25+ byte verbatim
 * substring of the loaded persona's `identity` (or `core_anchor`
 * fallback). Catches first-person identity leaks like `"i'm a Chief
 * Architect at Pure Health Solutions"` that G7 cannot catch (no name
 * in third-person construct). Case-insensitive. Disabled when
 * identity is NULL or shorter than 25 bytes. */

static void guard_g8_rejects_verbatim_identity_quote(void) {
    /* identity is 58 bytes; response quotes 35 bytes contiguously. */
    static const char identity[] = "51-year-old technical professional, lives alone with a cat";
    const char *raw = "yeah, i'm a technical professional, lives alone too lol";
    hu_guard_context_t ctx = {0};
    ctx.persona_identity = identity;
    ctx.persona_identity_len = sizeof(identity) - 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_persona_identity_echo);
}

static void guard_g8_rejects_chief_architect_phrase(void) {
    static const char identity[] = "Chief Architect at Pure Health Solutions";
    const char *raw = "yeah i'm a Chief Architect at Pure Health Solutions, busy day";
    hu_guard_context_t ctx = {0};
    ctx.persona_identity = identity;
    ctx.persona_identity_len = sizeof(identity) - 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_persona_identity_echo);
}

static void guard_g8_passes_short_overlap(void) {
    /* Identity has "senior software engineer" but response only shares
     * "i'm a senior" (12 bytes contiguous). 12 < 25 → no fire. */
    static const char identity[] = "a senior software engineer at the company";
    const char *raw = "i'm a senior dev";
    hu_guard_context_t ctx = {0};
    ctx.persona_identity = identity;
    ctx.persona_identity_len = sizeof(identity) - 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT(!report.detected_persona_identity_echo);
}

static void guard_g8_passes_when_identity_null(void) {
    const char *raw = "yeah i'm a Chief Architect at Pure Health Solutions";
    hu_guard_context_t ctx = {0};
    /* persona_identity = NULL by zero-init. */
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT(!report.detected_persona_identity_echo);
}

static void guard_g8_passes_when_identity_too_short(void) {
    /* 2-byte identity is below the 25-byte threshold. */
    static const char identity[] = "hi";
    const char *raw = "hi there, what's up";
    hu_guard_context_t ctx = {0};
    ctx.persona_identity = identity;
    ctx.persona_identity_len = sizeof(identity) - 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT(!report.detected_persona_identity_echo);
}

static void guard_g8_case_insensitive(void) {
    static const char identity[] = "chief architect at pure health solutions";
    const char *raw = "I'M A CHIEF ARCHITECT AT PURE HEALTH SOLUTIONS";
    hu_guard_context_t ctx = {0};
    ctx.persona_identity = identity;
    ctx.persona_identity_len = sizeof(identity) - 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_persona_identity_echo);
}

static void guard_g8_orthogonal_to_g6(void) {
    /* Both director_text and persona_identity are set. The response
     * quotes 30+ bytes of identity but does NOT quote the director.
     * Only G8 (not G6) should fire. */
    static const char director[] = "casual short, dry; respond briefly and slightly skeptical";
    static const char identity[] = "Chief Architect at Pure Health Solutions";
    const char *raw = "i'm a Chief Architect at Pure Health Solutions, what's up";
    hu_guard_context_t ctx = {0};
    ctx.director_text = director;
    ctx.director_len = sizeof(director) - 1;
    ctx.persona_identity = identity;
    ctx.persona_identity_len = sizeof(identity) - 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_persona_identity_echo);
    HU_ASSERT(!report.detected_director_echo);
}

static void guard_g8_orthogonal_to_g7(void) {
    /* Both persona_name and persona_identity set. The response quotes
     * 30 bytes of identity but does NOT contain the name in any
     * third-person construct. Only G8 (not G7) should fire. */
    static const char identity[] = "Chief Architect at Pure Health Solutions";
    const char *raw = "i'm a Chief Architect at Pure Health Solutions today";
    hu_guard_context_t ctx = {0};
    ctx.persona_name = "Seth";
    ctx.persona_name_len = 4;
    ctx.persona_identity = identity;
    ctx.persona_identity_len = sizeof(identity) - 1;
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ(outcome, HU_GUARD_REJECT);
    HU_ASSERT(report.detected_persona_identity_echo);
    HU_ASSERT(!report.detected_persona_pii_echo);
}

/* ── Sprint 33 — recent_assistant_avg_len helper ───────────────────────
 *
 * Production call sites (agent_stream.c, agent_turn.c) populate
 * `hu_guard_context_t.recent_avg_len` from this helper. These tests
 * pin the contract: empty / mixed-role / windowed cases. */

static void agent_recent_assistant_avg_len_empty_history_returns_zero(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    HU_ASSERT_EQ(hu_agent_internal_recent_assistant_avg_len(&agent, 5), 0u);

    /* NULL agent must also return 0 (defensive). */
    HU_ASSERT_EQ(hu_agent_internal_recent_assistant_avg_len(NULL, 5), 0u);

    /* max_n = 0 returns 0 even with content. */
    char body[] = "hello";
    hu_owned_message_t msgs[1];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = HU_ROLE_ASSISTANT;
    msgs[0].content = body;
    msgs[0].content_len = strlen(body);
    agent.history = msgs;
    agent.history_count = 1;
    HU_ASSERT_EQ(hu_agent_internal_recent_assistant_avg_len(&agent, 0), 0u);
}

static void agent_recent_assistant_avg_len_mixed_roles_skips_non_assistant(void) {
    char a1[] = "twelve bytes";       /* len=12 */
    char a2[] = "ten bytes!";         /* len=10 */
    char user[] = "user message big"; /* len=16, must be ignored */
    char tool[] = "tool result data"; /* len=16, must be ignored */
    char system[] = "system prompt";  /* len=13, must be ignored */

    hu_owned_message_t msgs[5];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = system;
    msgs[0].content_len = strlen(system);
    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = user;
    msgs[1].content_len = strlen(user);
    msgs[2].role = HU_ROLE_ASSISTANT;
    msgs[2].content = a1;
    msgs[2].content_len = strlen(a1);
    msgs[3].role = HU_ROLE_TOOL;
    msgs[3].content = tool;
    msgs[3].content_len = strlen(tool);
    msgs[4].role = HU_ROLE_ASSISTANT;
    msgs[4].content = a2;
    msgs[4].content_len = strlen(a2);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.history = msgs;
    agent.history_count = 5;

    /* EWMA over assistant lengths 12 then 10 → ~11. */
    HU_ASSERT_EQ(hu_agent_internal_recent_assistant_avg_len(&agent, 5), 11u);

    /* Empty-content assistant must be skipped. */
    char empty[] = "";
    msgs[2].content = empty;
    msgs[2].content_len = 0;
    /* Now only msgs[4] qualifies: avg = 10 / 1 = 10. */
    HU_ASSERT_EQ(hu_agent_internal_recent_assistant_avg_len(&agent, 5), 10u);
}

static void agent_recent_assistant_avg_len_uses_most_recent_n(void) {
    /* 7 assistant messages: 1000, 1000, 1000, 1000, 1000, 100, 100.
     * Newest 5 are: 1000, 1000, 100, 100, 1000 (walking back from the end).
     * Wait — newest-first walk is: msgs[6], msgs[5], msgs[4], msgs[3], msgs[2].
     * So newest 5 = 100, 100, 1000, 1000, 1000 → sum 3200, avg 640. */
    char *bodies[7];
    char buf1k[1001], buf100[101];
    memset(buf1k, 'x', 1000);
    buf1k[1000] = '\0';
    memset(buf100, 'y', 100);
    buf100[100] = '\0';

    bodies[0] = buf1k;
    bodies[1] = buf1k;
    bodies[2] = buf1k;
    bodies[3] = buf1k;
    bodies[4] = buf1k;
    bodies[5] = buf100;
    bodies[6] = buf100;

    hu_owned_message_t msgs[7];
    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < 7; i++) {
        msgs[i].role = HU_ROLE_ASSISTANT;
        msgs[i].content = bodies[i];
        msgs[i].content_len = strlen(bodies[i]);
    }

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.history = msgs;
    agent.history_count = 7;

    /* Newest 5 (chronological 1k×3, 100×2) with EWMA α=0.35 → ~480. */
    HU_ASSERT_EQ(hu_agent_internal_recent_assistant_avg_len(&agent, 5), 480u);

    /* Newest 2: both 100 → 100. */
    HU_ASSERT_EQ(hu_agent_internal_recent_assistant_avg_len(&agent, 2), 100u);

    /* All 7: oldest three are also 1k, so EWMA converges to the same ~480
     * as newest-5 (not the arithmetic mean ~743). */
    HU_ASSERT_EQ(hu_agent_internal_recent_assistant_avg_len(&agent, 100), 480u);

    /* Including short oldest turns changes EWMA vs newest-5 only. */
    bodies[0] = buf100;
    bodies[1] = buf100;
    msgs[0].content = buf100;
    msgs[0].content_len = 100;
    msgs[1].content = buf100;
    msgs[1].content_len = 100;
    HU_ASSERT_EQ(hu_agent_internal_recent_assistant_avg_len(&agent, 5), 480u);
    HU_ASSERT_EQ(hu_agent_internal_recent_assistant_avg_len(&agent, 100), 375u);
}

/* ── Critique-echo predicate (D1, 2026-05-19) ─────────────────────────── */
/* Tests the predicate that catches when the LLM echoes the reflection
 * critique back as its retry response — covering both the dpo write-side
 * guard (src/ml/dpo.c) and the agent_turn user-facing scrub
 * (src/agent/agent_turn.c). The predicate IS the contract: testing it
 * in isolation per ~/.claude/rules/security-predicate-extraction.md is
 * worth more than spawning a full agent with a mock provider. */

static void critique_echo_predicate_detects_NEEDS_RETRY_prefix(void) {
    /* The exact shape seen in 9/10 reflection_retry audit rows. */
    const char *echo = "NEEDS_RETRY. The response 'GOOD' is irrelevant to the prompt.";
    HU_ASSERT_TRUE(hu_response_is_critique_echo(echo, strlen(echo)));
}

static void critique_echo_predicate_detects_needs_retry_lowercase_prefix(void) {
    /* Some quality evaluators emit lowercase. Catch that too. */
    const char *echo = "needs_retry: too short and missing context";
    HU_ASSERT_TRUE(hu_response_is_critique_echo(echo, strlen(echo)));
}

static void critique_echo_predicate_allows_embedded_mention(void) {
    /* A legitimate response that mentions NEEDS_RETRY in its body
     * (not as prefix) is fine — only the prefix is the bug shape. */
    const char *ok = "I evaluated the prior NEEDS_RETRY case earlier and moved on.";
    HU_ASSERT_FALSE(hu_response_is_critique_echo(ok, strlen(ok)));
}

static void critique_echo_predicate_allows_legitimate_response(void) {
    /* Normal Seth-shape iMessage reply — must not be misclassified. */
    const char *ok = "yeah let me look at it";
    HU_ASSERT_FALSE(hu_response_is_critique_echo(ok, strlen(ok)));
}

static void critique_echo_predicate_handles_short_input(void) {
    /* Shorter than the prefix length — must not memcmp-overrun. */
    HU_ASSERT_FALSE(hu_response_is_critique_echo("NEEDS", 5));
    HU_ASSERT_FALSE(hu_response_is_critique_echo("", 0));
    /* Boundary: exactly 11 bytes equal to "NEEDS_RETRY" — counts as echo. */
    HU_ASSERT_TRUE(hu_response_is_critique_echo("NEEDS_RETRY", 11));
}

static void critique_echo_predicate_handles_null_safe(void) {
    /* Null s must not crash. */
    HU_ASSERT_FALSE(hu_response_is_critique_echo(NULL, 0));
    HU_ASSERT_FALSE(hu_response_is_critique_echo(NULL, 100));
}

/* ── Sprint 41 — G9 naked discourse-marker opener ─────────────────────── */

/* The exact production message that prompted this guard. Pure-predicate
 * level: starts with "tbh", then space, then "morning", then ".", then a
 * follow-up like "you awake yet?". The discourse marker "tbh" has nothing
 * to be honest ABOUT — backchannel markers require contrast with a prior
 * proposition. The LoRA adapter learned "tbh" as a high-probability
 * sentence-opener from raw Seth corpus and pasted it onto a greeting. */
static void g9_rejects_production_jordan_tbh_morning_leak(void) {
    const char *raw = "tbh morning. you awake yet?";
    HU_ASSERT_TRUE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

static void g9_rejects_ngl_hey_grab_coffee(void) {
    const char *raw = "ngl hey wanna grab coffee?";
    HU_ASSERT_TRUE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

static void g9_rejects_imo_morning_alone(void) {
    /* Marker + greeting with NO trailing clause at all. */
    const char *raw = "imo morning";
    HU_ASSERT_TRUE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

static void g9_rejects_honestly_yo_check_in(void) {
    const char *raw = "honestly yo just checking in";
    HU_ASSERT_TRUE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

static void g9_rejects_lowkey_sup_short(void) {
    const char *raw = "lowkey sup?";
    HU_ASSERT_TRUE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

/* Counter-examples: marker followed by a clause-completing copula verb
 * makes the marker pragmatically legitimate. "tbh morning IS the worst"
 * means "to be honest, morning is bad" — that's a real opinion. The
 * predicate must NOT fire on these. */
static void g9_allows_marker_followed_by_clause_about_morning(void) {
    const char *raw = "tbh morning is the worst part of the day";
    HU_ASSERT_FALSE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

static void g9_allows_marker_followed_by_idk(void) {
    /* "tbh idk" is canonical real-Seth texting — marker + clausal
     * shorthand. Must not be rejected. */
    const char *raw = "tbh idk what to do";
    HU_ASSERT_FALSE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

static void g9_allows_plain_greeting_no_marker(void) {
    const char *raw = "morning! you awake yet?";
    HU_ASSERT_FALSE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

static void g9_allows_marker_only_no_greeting(void) {
    const char *raw = "tbh that was a wild day";
    HU_ASSERT_FALSE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

static void g9_allows_greeting_inside_message_not_at_start(void) {
    /* G9 fires only when the marker+greeting is at the START. Embedded
     * mention is fine. */
    const char *raw = "yeah I texted them tbh morning was rough";
    HU_ASSERT_FALSE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

static void g9_allows_morningstar_word_not_morning(void) {
    /* Word-boundary check: "morningstar" must not match "morning". */
    const char *raw = "tbh morningstar is a cool name";
    HU_ASSERT_FALSE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

static void g9_handles_null_safe(void) {
    HU_ASSERT_FALSE(hu_response_is_naked_discourse_opener(NULL, 0));
    HU_ASSERT_FALSE(hu_response_is_naked_discourse_opener(NULL, 100));
    HU_ASSERT_FALSE(hu_response_is_naked_discourse_opener("", 0));
}

static void g9_handles_leading_whitespace(void) {
    /* Some providers prepend a newline before the response. The predicate
     * should still trigger after skipping leading whitespace. */
    const char *raw = "  \n  tbh morning. you awake yet?";
    HU_ASSERT_TRUE(hu_response_is_naked_discourse_opener(raw, strlen(raw)));
}

/* End-to-end: the full hu_response_guard_check_ex path REJECTs the
 * production Jordan message and populates the report flag. */
static void g9_check_ex_rejects_jordan_case_with_report(void) {
    const char *raw = "tbh morning. you awake yet?";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);
    HU_ASSERT_EQ((int)outcome, (int)HU_GUARD_REJECT);
    HU_ASSERT_TRUE(report.detected_naked_discourse_opener);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, (size_t)0);
}

static void g9_check_ex_increments_reject_stats(void) {
    hu_guard_reject_stats_reset();
    hu_guard_reject_stats_t before;
    hu_guard_reject_stats_snapshot(&before);

    const char *raw = "tbh morning. you awake yet?";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    (void)hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report);

    hu_guard_reject_stats_t after;
    hu_guard_reject_stats_snapshot(&after);
    HU_ASSERT_EQ((after.naked_discourse_opener - before.naked_discourse_opener), (uint64_t)1);
}

/* ── Sprint 41 follow-up — G9 escape hatches ──────────────────────────── */

static void g9_per_call_disabled_lets_jordan_case_through(void) {
    const char *raw = "tbh morning. you awake yet?";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    hu_guard_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.naked_opener_disabled = true; /* per-call escape hatch */
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ((int)outcome, (int)HU_GUARD_OK);
    HU_ASSERT_FALSE(report.detected_naked_discourse_opener);
}

static void g9_global_kill_switch_lets_jordan_case_through(void) {
    /* Save + restore the global flag so test ordering doesn't matter. */
    bool was_disabled = hu_response_guard_naked_opener_globally_disabled();
    hu_response_guard_set_naked_opener_globally_disabled(true);

    const char *raw = "tbh morning. you awake yet?";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, raw, strlen(raw), &out, &out_len, &outcome, &report),
        HU_OK);
    HU_ASSERT_EQ((int)outcome, (int)HU_GUARD_OK);
    HU_ASSERT_FALSE(report.detected_naked_discourse_opener);

    hu_response_guard_set_naked_opener_globally_disabled(was_disabled);
}

static void g9_global_kill_switch_round_trip(void) {
    bool initial = hu_response_guard_naked_opener_globally_disabled();
    hu_response_guard_set_naked_opener_globally_disabled(true);
    HU_ASSERT_TRUE(hu_response_guard_naked_opener_globally_disabled());
    hu_response_guard_set_naked_opener_globally_disabled(false);
    HU_ASSERT_FALSE(hu_response_guard_naked_opener_globally_disabled());
    hu_response_guard_set_naked_opener_globally_disabled(initial);
}

/* ── Sprint 41 follow-up — DPO negative-pair JSONL formatter ──────────── */

#include "human/agent/response_guard_dpo.h"

static void dpo_format_emits_well_formed_jsonl_for_jordan_case(void) {
    char buf[1024];
    size_t n = hu_response_guard_format_dpo_negative_jsonl(
        "hey you up?", strlen("hey you up?"), "tbh morning. you awake yet?",
        strlen("tbh morning. you awake yet?"), "naked_discourse_opener", "imessage",
        (int64_t)1779800000, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "\"prompt\":\"hey you up?\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"chosen\":null") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"rejected\":\"tbh morning. you awake yet?\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"_detector\":\"naked_discourse_opener\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"_channel\":\"imessage\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"_ts_unix\":1779800000") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"_source\":\"response_guard\"") != NULL);
}

static void dpo_format_escapes_quotes_and_newlines(void) {
    char buf[512];
    /* Rejected text with embedded quote + newline + backslash — must escape. */
    const char *rejected = "say \"hi\"\nthen \\bye";
    size_t n = hu_response_guard_format_dpo_negative_jsonl("hi", 2, rejected, strlen(rejected),
                                                           "test_detector", "test_channel",
                                                           (int64_t)1, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "\\\"hi\\\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\\n") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\\\\bye") != NULL);
    /* The raw embedded newline must NOT appear unescaped. */
    HU_ASSERT_NULL(strchr(buf, '\n'));
}

static void dpo_format_emits_null_for_null_strings(void) {
    char buf[512];
    size_t n = hu_response_guard_format_dpo_negative_jsonl(NULL, 0, NULL, 0, NULL, NULL,
                                                           (int64_t)42, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "\"prompt\":null") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"rejected\":null") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"_detector\":null") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"_channel\":null") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"_ts_unix\":42") != NULL);
}

static void dpo_format_truncates_safely_on_overflow(void) {
    /* A small buffer cannot fit a full record — formatter must NUL-terminate
     * and return a length < cap (so the caller can detect truncation by
     * comparing return against cap-1). Use a guard buffer pattern: place
     * the under-test buffer in the MIDDLE of a larger memset'd region and
     * confirm bytes outside `[0..cap)` are untouched. */
    char wrap[64];
    memset(wrap, 0xAA, sizeof(wrap));
    char *buf = wrap + 16; /* 16-byte canary on each side */
    const size_t cap = 32;
    size_t n = hu_response_guard_format_dpo_negative_jsonl(
        "long prompt that overflows the small buffer", 43, "long rejected text that also overflows",
        38, "naked_discourse_opener", "imessage", (int64_t)1779800000, buf, cap);
    /* Returned length must fit in cap and NUL-terminate. */
    HU_ASSERT_TRUE(n < cap);
    HU_ASSERT_EQ(buf[n], '\0');
    /* Canary bytes BEFORE and AFTER the buffer must still be 0xAA. */
    for (size_t i = 0; i < 16; i++) {
        HU_ASSERT_EQ((unsigned char)wrap[i], (unsigned char)0xAA);
        HU_ASSERT_EQ((unsigned char)wrap[16 + cap + i], (unsigned char)0xAA);
    }
}

static void dpo_format_returns_zero_on_zero_capacity(void) {
    char buf[8];
    size_t n = hu_response_guard_format_dpo_negative_jsonl("p", 1, "r", 1, "d", "c", 1, buf, 0);
    HU_ASSERT_EQ(n, (size_t)0);
}

static void dpo_log_is_noop_under_hu_is_test(void) {
    /* In the test build, the writer is a no-op returning HU_OK regardless
     * of HOME or filesystem state. This pins that contract so prod and
     * test paths cannot accidentally diverge. */
    hu_error_t err = hu_response_guard_log_dpo_negative("p", 1, "r", 1, "d", "c", (int64_t)1);
    HU_ASSERT_EQ(err, HU_OK);
}

/* ── Sprint 41 follow-up #3 — G9 retry-outcome telemetry ─────────────── */

static void g9_retry_outcome_rescued_increments_correct_counter(void) {
    hu_guard_reject_stats_reset();
    hu_response_guard_record_g9_retry_outcome(/*retry_succeeded=*/true,
                                              /*retry_tripped_g9_again=*/false);
    hu_guard_reject_stats_t s;
    hu_guard_reject_stats_snapshot(&s);
    HU_ASSERT_EQ(s.g9_retry_rescued, (uint64_t)1);
    HU_ASSERT_EQ(s.g9_retry_thrashed, (uint64_t)0);
    HU_ASSERT_EQ(s.g9_retry_starved, (uint64_t)0);
}

static void g9_retry_outcome_thrashed_when_retry_trips_g9_again(void) {
    hu_guard_reject_stats_reset();
    hu_response_guard_record_g9_retry_outcome(/*retry_succeeded=*/true,
                                              /*retry_tripped_g9_again=*/true);
    hu_guard_reject_stats_t s;
    hu_guard_reject_stats_snapshot(&s);
    HU_ASSERT_EQ(s.g9_retry_rescued, (uint64_t)0);
    HU_ASSERT_EQ(s.g9_retry_thrashed, (uint64_t)1);
    HU_ASSERT_EQ(s.g9_retry_starved, (uint64_t)0);
}

static void g9_retry_outcome_starved_when_retry_failed(void) {
    hu_guard_reject_stats_reset();
    hu_response_guard_record_g9_retry_outcome(/*retry_succeeded=*/false,
                                              /*retry_tripped_g9_again=*/false);
    hu_guard_reject_stats_t s;
    hu_guard_reject_stats_snapshot(&s);
    HU_ASSERT_EQ(s.g9_retry_starved, (uint64_t)1);
}

/* ── Sprint 41 follow-up #3 — DPO daily-rotated path ────────────────── */

#include "human/agent/response_guard_dpo.h"

static void dpo_path_for_day_emits_utc_dated_filename(void) {
    /* 1779840000 = 2026-05-27 00:00:00 UTC (gmtime-verified). */
    char buf[256];
    size_t n =
        hu_response_guard_dpo_path_for_day("/home/seth", (int64_t)1779840000, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, "/home/seth/.human/training-data/m3-dpo-rejections-2026-05-27.jsonl");
}

static void dpo_path_for_day_rolls_at_utc_midnight(void) {
    /* 2026-05-27 23:59:59 UTC = 1779840000 + 86399 → same day.
     * 2026-05-28 00:00:00 UTC = 1779840000 + 86400 → next day. */
    char before[256];
    char after[256];
    (void)hu_response_guard_dpo_path_for_day("/h", (int64_t)1779840000 + 86399, before,
                                             sizeof(before));
    (void)hu_response_guard_dpo_path_for_day("/h", (int64_t)1779840000 + 86400, after,
                                             sizeof(after));
    HU_ASSERT_TRUE(strstr(before, "2026-05-27") != NULL);
    HU_ASSERT_TRUE(strstr(after, "2026-05-28") != NULL);
}

static void dpo_path_for_day_returns_zero_on_null_home(void) {
    char buf[64];
    HU_ASSERT_EQ(hu_response_guard_dpo_path_for_day(NULL, 1779840000, buf, sizeof(buf)), (size_t)0);
    HU_ASSERT_EQ(hu_response_guard_dpo_path_for_day("", 1779840000, buf, sizeof(buf)), (size_t)0);
}

static void dpo_path_for_day_returns_zero_on_tiny_buffer(void) {
    char tiny[16];
    HU_ASSERT_EQ(
        hu_response_guard_dpo_path_for_day("/home/seth/long/path", 1779840000, tiny, sizeof(tiny)),
        (size_t)0);
}

/* ── Sprint 41 follow-up #4 — per-channel G9 disable list ─────────────── */

static void g9_disabled_for_channel_returns_false_when_list_empty(void) {
    hu_response_guard_set_g9_disabled_channels(NULL, 0);
    HU_ASSERT_FALSE(hu_response_guard_g9_disabled_for_channel("imessage", 8));
}

static void g9_disabled_for_channel_matches_listed_channel(void) {
    const char *list[] = {"voice", "discord"};
    hu_response_guard_set_g9_disabled_channels(list, 2);
    HU_ASSERT_TRUE(hu_response_guard_g9_disabled_for_channel("voice", 5));
    HU_ASSERT_TRUE(hu_response_guard_g9_disabled_for_channel("discord", 7));
    HU_ASSERT_FALSE(hu_response_guard_g9_disabled_for_channel("imessage", 8));
    hu_response_guard_set_g9_disabled_channels(NULL, 0); /* clean up */
}

static void g9_disabled_for_channel_is_case_insensitive(void) {
    const char *list[] = {"Voice"};
    hu_response_guard_set_g9_disabled_channels(list, 1);
    HU_ASSERT_TRUE(hu_response_guard_g9_disabled_for_channel("voice", 5));
    HU_ASSERT_TRUE(hu_response_guard_g9_disabled_for_channel("VOICE", 5));
    hu_response_guard_set_g9_disabled_channels(NULL, 0);
}

static void g9_disabled_for_channel_does_not_partial_match(void) {
    /* Channel "voice" must not match "voicebox" or "vo" — whole-name only. */
    const char *list[] = {"voice"};
    hu_response_guard_set_g9_disabled_channels(list, 1);
    HU_ASSERT_FALSE(hu_response_guard_g9_disabled_for_channel("voicebox", 8));
    HU_ASSERT_FALSE(hu_response_guard_g9_disabled_for_channel("vo", 2));
    hu_response_guard_set_g9_disabled_channels(NULL, 0);
}

static void g9_disabled_for_channel_jordan_case_bypassed_when_channel_listed(void) {
    /* End-to-end via hu_response_guard_check_ex with ctx.naked_opener_disabled
     * set, simulating what agent_turn does after consulting the channel list. */
    const char *raw = "tbh morning. you awake yet?";
    hu_allocator_t alloc = A();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    hu_guard_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* Mirror agent_turn: caller consults the list and sets the bit. */
    const char *list[] = {"voice"};
    hu_response_guard_set_g9_disabled_channels(list, 1);
    ctx.naked_opener_disabled = hu_response_guard_g9_disabled_for_channel("voice", 5);
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, raw, strlen(raw), &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ((int)outcome, (int)HU_GUARD_OK);
    HU_ASSERT_FALSE(report.detected_naked_discourse_opener);
    hu_response_guard_set_g9_disabled_channels(NULL, 0);
}

/* ── Task 10 (AC-9): Learned per-contact G5 baseline ─────────────────── */

static void g5_learned_baseline_within_normal_range_passes(void) {
    /* AC-9: Seth-normal length to a contact should pass when within the
     * learned baseline. If Seth habitually sends 500-char messages to a
     * contact, a 400-char reply should not trip G5. */
    hu_allocator_t alloc = A();
    const char *response = "This is a reasonable message that Seth might send to a "
                           "contact he talks to regularly. It's around 200 chars long "
                           "and should be perfectly fine because the contact is used to "
                           "getting messages of this length.";
    size_t response_len = strlen(response);

    hu_guard_context_t ctx = {0};
    ctx.learned_avg_message_length = 250; /* Seth sends 250-char messages to this contact */
    ctx.recent_avg_len = 50;              /* rolling avg would flag this as anomalous (4x) */

    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report = {0};

    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, response, response_len, &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ((int)outcome, (int)HU_GUARD_OK);
    HU_ASSERT_FALSE(report.detected_length_anomaly);
}

static void g5_learned_baseline_genuinely_anomalous_still_rejected(void) {
    /* AC-9: a genuinely huge dump (5x learned baseline) should still be
     * rejected, showing the learned baseline doesn't disable length checking,
     * just relaxes it for normal contact behavior. */
    hu_allocator_t alloc = A();
    const char *response = "This is a deliberately enormous response designed to be way "
                           "bigger than the learned baseline. " /* ~100 chars so far */
                           "The contact usually gets 250-char messages, but we're sending "
                           "800 characters here, which is 3x the learned baseline and should "
                           "be rejected as a length anomaly. This simulates a context dump or "
                           "prompt leak that happens to come from the model despite having a "
                           "learned baseline context. The absolute floor (320 chars) allows "
                           "legitimate but longish messages, but 800 is way outside normal. "
                           "This entire string is designed to exceed 800 characters to trigger "
                           "the anomaly rejection, even with the learned baseline in place. "
                           "We pad it further to be certain the byte count clears the 800 mark: "
                           "the guard must treat a payload of this magnitude as a clear outlier "
                           "regardless of how chatty the contact normally is, because no human "
                           "texting a friend dumps this many characters in a single turn. The "
                           "learned baseline relaxes the threshold for normal variation, not for "
                           "a wall of text like this one, which is unmistakably a context leak.";
    size_t response_len = strlen(response);
    HU_ASSERT(response_len > 800); /* verify test setup */

    hu_guard_context_t ctx = {0};
    ctx.learned_avg_message_length = 250; /* Seth sends 250-char messages to this contact */

    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report = {0};

    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, response, response_len, &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ((int)outcome, (int)HU_GUARD_REJECT);
    HU_ASSERT_TRUE(report.detected_length_anomaly);
}

static void g5_without_learned_baseline_falls_back_to_rolling_avg(void) {
    /* AC-9: when learned_avg_message_length is 0 (not available), fall back to
     * the rolling average + multiplier. This ensures backward compatibility
     * when no personal model data exists. */
    hu_allocator_t alloc = A();
    const char *response = "Short reply";
    size_t response_len = strlen(response);

    hu_guard_context_t ctx = {0};
    ctx.learned_avg_message_length = 0; /* no learned baseline */
    ctx.recent_avg_len = 50;
    ctx.length_anomaly_mult = 8;

    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report = {0};

    /* 11 chars < 320 floor, so passes despite being above floor + mult calc */
    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, response, response_len, &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ((int)outcome, (int)HU_GUARD_OK);
    HU_ASSERT_FALSE(report.detected_length_anomaly);
}

static void g5_absolute_floor_prevents_death_spiral(void) {
    /* AC-9: the absolute floor (320 chars) prevents the forced-short →
     * low-avg → reject death-spiral. Even with a near-zero learned baseline,
     * natural-length messages below 320 should not be rejected. */
    hu_allocator_t alloc = A();
    const char *response = "This is a completely normal response that's maybe 100 chars "
                           "or so, which is a very reasonable message. It should definitely "
                           "not be rejected even if the contact is used to super short msgs.";
    size_t response_len = strlen(response);
    HU_ASSERT(response_len < 320); /* verify floor */

    hu_guard_context_t ctx = {0};
    ctx.learned_avg_message_length = 10; /* contact sends VERY short messages */

    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report = {0};

    HU_ASSERT_EQ(hu_response_guard_check_ex(&alloc, response, response_len, &ctx, &out, &out_len,
                                            &outcome, &report),
                 HU_OK);
    HU_ASSERT_EQ((int)outcome, (int)HU_GUARD_OK);
    HU_ASSERT_FALSE(report.detected_length_anomaly);
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
    HU_RUN_TEST(guard_rejects_msg_56354_brea_primary_recipient_leak_verbatim);
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

    /* Sprint 32 — additional verbatim leaks surfaced by audit script. */
    HU_RUN_TEST(guard_rejects_msg_56049_user_constraints_scene_direction_leak_verbatim);
    HU_RUN_TEST(guard_rejects_msg_56063_persona_block_short_leak_verbatim);

    /* Sprint 31 — context-aware detections (G5 length anomaly, G6
     * director echo). All exercise the new `_ex` API. */
    HU_RUN_TEST(guard_length_mult_for_channel_compact_vs_default);
    HU_RUN_TEST(guard_g5_imessage_channel_uses_stricter_mult);
    HU_RUN_TEST(guard_g5_does_not_fire_below_absolute_floor);
    HU_RUN_TEST(guard_ex_rejects_length_anomaly);
    HU_RUN_TEST(guard_ex_rejects_director_echo);
    HU_RUN_TEST(guard_ex_passes_long_response_when_no_avg);
    HU_RUN_TEST(guard_ex_passes_legit_5x_response);
    HU_RUN_TEST(guard_ex_passes_short_director_text);
    HU_RUN_TEST(guard_ex_with_null_ctx_matches_legacy_behavior);

    /* Sprint 33 — recent_assistant_avg_len helper used by production
     * call sites (agent_stream.c, agent_turn.c) to populate
     * `hu_guard_context_t.recent_avg_len` and enforce G5 at runtime. */
    HU_RUN_TEST(agent_recent_assistant_avg_len_empty_history_returns_zero);
    HU_RUN_TEST(agent_recent_assistant_avg_len_mixed_roles_skips_non_assistant);
    HU_RUN_TEST(agent_recent_assistant_avg_len_uses_most_recent_n);

    /* Sprint 35 — persona-PII echo (G7). Catches third-person profile
     * constructs ("<Name> is a", "<Name>'s job", "<Name> lives") that
     * leak the loaded persona's identity. */
    HU_RUN_TEST(guard_g7_rejects_long_parenthetical_before_is_a);
    HU_RUN_TEST(guard_g7_rejects_third_person_is_construct);
    HU_RUN_TEST(guard_g7_rejects_third_person_possessive);
    HU_RUN_TEST(guard_g7_rejects_third_person_lives_works);
    HU_RUN_TEST(guard_g7_passes_first_person_self_reference);
    HU_RUN_TEST(guard_g7_passes_direct_address);
    HU_RUN_TEST(guard_g7_skips_when_persona_name_null);
    HU_RUN_TEST(guard_g7_skips_when_persona_name_too_short);
    HU_RUN_TEST(guard_g7_word_boundary_isolates_name);
    HU_RUN_TEST(guard_g7_case_insensitive);

    /* Sprint 36 — persona identity / core-anchor echo (G8). Catches
     * verbatim 25+ byte substrings of the loaded persona's biographical
     * identity string in the response, regardless of whether the name
     * appears (covers first-person identity leaks). */
    HU_RUN_TEST(guard_g8_rejects_verbatim_identity_quote);
    HU_RUN_TEST(guard_g8_rejects_chief_architect_phrase);
    HU_RUN_TEST(guard_g8_passes_short_overlap);
    HU_RUN_TEST(guard_g8_passes_when_identity_null);
    HU_RUN_TEST(guard_g8_passes_when_identity_too_short);
    HU_RUN_TEST(guard_g8_case_insensitive);
    HU_RUN_TEST(guard_g8_orthogonal_to_g6);
    HU_RUN_TEST(guard_g8_orthogonal_to_g7);

    /* Sprint 37 — Cross-turn director-history echo (G6 extension). G6
     * now iterates a small ring buffer of recent directors so a model
     * that quotes yesterday's director instead of today's still trips. */
    HU_RUN_TEST(guard_g6_history_catches_previous_director);
    HU_RUN_TEST(guard_g6_history_skips_below_threshold);
    HU_RUN_TEST(guard_g6_history_orthogonal_to_current);
    HU_RUN_TEST(guard_g6_history_zero_count_disables);
    HU_RUN_TEST(guard_g6_history_walks_all_slots);

    /* Sprint 37 — Director history ring buffer helpers used by the
     * daemon to carry the about-to-go-stale director across turns. */
    HU_RUN_TEST(agent_director_history_push_basic);
    HU_RUN_TEST(agent_director_history_push_overflow_evicts_oldest);
    HU_RUN_TEST(agent_director_history_push_truncates_long);
    HU_RUN_TEST(agent_director_history_push_null_is_noop);
    HU_RUN_TEST(agent_director_history_free_zeroes_count);

    /* Sprint 40 — cross-recipient hygiene + selection-step audit. */
    HU_RUN_TEST(agent_contact_boundary_clears_director_history);
    HU_RUN_TEST(guard_audit_detects_numbered_analysis_fixture);
    HU_RUN_TEST(guard_d8_rejects_assistant_service_phrasing);
    HU_RUN_TEST(guard_d8_allows_human_help_offers);

    /* Sprint 38 — G8 biography source + reject telemetry counters. */
    HU_RUN_TEST(guard_g8_rejects_biography_only_echo);
    HU_RUN_TEST(guard_g8_orthogonal_biography_vs_identity);
    HU_RUN_TEST(guard_reject_stats_g8_increments_on_identity_echo);
    HU_RUN_TEST(guard_reject_stats_reset_clears_counters);
    HU_RUN_TEST(critique_echo_predicate_detects_NEEDS_RETRY_prefix);
    HU_RUN_TEST(critique_echo_predicate_detects_needs_retry_lowercase_prefix);
    HU_RUN_TEST(critique_echo_predicate_allows_embedded_mention);
    HU_RUN_TEST(critique_echo_predicate_allows_legitimate_response);
    HU_RUN_TEST(critique_echo_predicate_handles_short_input);
    HU_RUN_TEST(critique_echo_predicate_handles_null_safe);

    /* Sprint 41 — G9 naked discourse-marker opener (SPRINT41_DATE_LATER Jordan
     * incident: production sent "tbh morning. you awake yet?" to a real
     * contact, an unsemantic concatenation produced by the LoRA adapter
     * that learned discourse markers as high-probability sentence-
     * openers from raw Seth texting). */
    HU_RUN_TEST(g9_rejects_production_jordan_tbh_morning_leak);
    HU_RUN_TEST(g9_rejects_ngl_hey_grab_coffee);
    HU_RUN_TEST(g9_rejects_imo_morning_alone);
    HU_RUN_TEST(g9_rejects_honestly_yo_check_in);
    HU_RUN_TEST(g9_rejects_lowkey_sup_short);
    HU_RUN_TEST(g9_allows_marker_followed_by_clause_about_morning);
    HU_RUN_TEST(g9_allows_marker_followed_by_idk);
    HU_RUN_TEST(g9_allows_plain_greeting_no_marker);
    HU_RUN_TEST(g9_allows_marker_only_no_greeting);
    HU_RUN_TEST(g9_allows_greeting_inside_message_not_at_start);
    HU_RUN_TEST(g9_allows_morningstar_word_not_morning);
    HU_RUN_TEST(g9_handles_null_safe);
    HU_RUN_TEST(g9_handles_leading_whitespace);
    HU_RUN_TEST(g9_check_ex_rejects_jordan_case_with_report);
    HU_RUN_TEST(g9_check_ex_increments_reject_stats);

    /* Sprint 41 follow-up — G9 escape hatches + DPO logging. */
    HU_RUN_TEST(g9_per_call_disabled_lets_jordan_case_through);
    HU_RUN_TEST(g9_global_kill_switch_lets_jordan_case_through);
    HU_RUN_TEST(g9_global_kill_switch_round_trip);
    HU_RUN_TEST(dpo_format_emits_well_formed_jsonl_for_jordan_case);
    HU_RUN_TEST(dpo_format_escapes_quotes_and_newlines);
    HU_RUN_TEST(dpo_format_emits_null_for_null_strings);
    HU_RUN_TEST(dpo_format_truncates_safely_on_overflow);
    HU_RUN_TEST(dpo_format_returns_zero_on_zero_capacity);
    HU_RUN_TEST(dpo_log_is_noop_under_hu_is_test);

    /* Sprint 41 follow-up #3 — G9 retry-outcome telemetry. */
    HU_RUN_TEST(g9_retry_outcome_rescued_increments_correct_counter);
    HU_RUN_TEST(g9_retry_outcome_thrashed_when_retry_trips_g9_again);
    HU_RUN_TEST(g9_retry_outcome_starved_when_retry_failed);

    /* Sprint 41 follow-up #3 — DPO daily-rotated path. */
    HU_RUN_TEST(dpo_path_for_day_emits_utc_dated_filename);
    HU_RUN_TEST(dpo_path_for_day_rolls_at_utc_midnight);
    HU_RUN_TEST(dpo_path_for_day_returns_zero_on_null_home);
    HU_RUN_TEST(dpo_path_for_day_returns_zero_on_tiny_buffer);

    /* Sprint 41 follow-up #4 — per-channel G9 disable list. */
    HU_RUN_TEST(g9_disabled_for_channel_returns_false_when_list_empty);
    HU_RUN_TEST(g9_disabled_for_channel_matches_listed_channel);
    HU_RUN_TEST(g9_disabled_for_channel_is_case_insensitive);
    HU_RUN_TEST(g9_disabled_for_channel_does_not_partial_match);
    HU_RUN_TEST(g9_disabled_for_channel_jordan_case_bypassed_when_channel_listed);

    /* Task 10 (AC-9) — learned per-contact G5 baseline. */
    HU_RUN_TEST(g5_learned_baseline_within_normal_range_passes);
    HU_RUN_TEST(g5_learned_baseline_genuinely_anomalous_still_rejected);
    HU_RUN_TEST(g5_without_learned_baseline_falls_back_to_rolling_avg);
    HU_RUN_TEST(g5_absolute_floor_prevents_death_spiral);
}
