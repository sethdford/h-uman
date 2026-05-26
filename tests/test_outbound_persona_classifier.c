/* tests/test_outbound_persona_classifier.c
 *
 * Sprint 60 follow-up to Sprint 59 — wires the outbound persona stage
 * to the deterministic shape classifier from include/human/eval/shape.h
 * so the gate isn't limited to the heuristic project-jargon /
 * AI-self-aware blocklists. Closes item #1 of
 * docs/plans/2026-05-26-sprint-59-outbound-safety/STATUS.md.
 *
 * Layering:
 *   1. Existing heuristic blocklist (still runs first; corpus #11-18)
 *   2. NEW: hu_shape_classify against the channel's rules. If
 *      shape_result.score < 0.5 (per design.md Q-3), REGENERATE with a
 *      hint pointing at the dominant failure flag.
 *
 * Per Q-3 user decision: REGENERATE only (never REJECT). False
 * positives must not drop legitimate messages.
 *
 * Threshold rationale (design.md): empirical persona-fidelity eval
 * showed v4-repair adapter lifted Seth-voice score 0.586 → 0.856
 * (+27pp). 0.5 is a clear "this doesn't match Seth at all" floor;
 * anything above is plausible-voice and goes to SEND.
 */

#include "test_framework.h"

#include "human/agent/outbound_pipeline.h"
#include "human/core/allocator.h"
#include <stdlib.h>
#include <string.h>

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

static hu_outbound_verdict_t run_persona_with_channel(const char *content, const char *channel) {
    hu_outbound_message_t msg = {0};
    msg.content = (char *)content;
    msg.content_len = strlen(content);

    hu_outbound_context_t ctx = {0};
    ctx.alloc = test_alloc();
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.regenerate_budget = 1;
    ctx.channel_name = channel;
    ctx.recipient_contact_id = "+TEST_RECIPIENT";
    ctx.recipient_contact_id_len = strlen("+TEST_RECIPIENT");

    return hu_outbound_stage_persona.run(&hu_outbound_stage_persona, &msg, &ctx);
}

/* ── Shape-classifier-driven REGENERATEs ──────────────────────────── */

/* AI-assistant opener — the canonical "Certainly!" / "Absolutely!" /
 * "Great question!" pattern. The existing heuristic blocklist does
 * NOT catch these (they're not in PROJECT_JARGON or AI_SELF_AWARE),
 * but hu_shape_classify scores them at < 0.5 via the FAIL_CERTAINLY
 * / FAIL_GREAT_QUESTION flags. */
static void test_classifier_catches_certainly_opener(void) {
    hu_outbound_verdict_t v =
        run_persona_with_channel("Certainly! I can help with that.", "imessage");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
    HU_ASSERT_NOT_NULL(v.reason);
    HU_ASSERT_TRUE(strstr(v.reason, "persona_shape") != NULL);
}

static void test_classifier_catches_great_question_opener(void) {
    hu_outbound_verdict_t v =
        run_persona_with_channel("Great question! Let me think about that.", "imessage");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
    HU_ASSERT_TRUE(strstr(v.reason, "persona_shape") != NULL);
}

/* "Here are" + bullet list — fails the iMessage strict shape gate via
 * HERE_ARE + BULLET_LIST flags. */
static void test_classifier_catches_here_are_bullet_list(void) {
    const char *bullets = "Here are some options:\n- option one\n- option two\n- option three";
    hu_outbound_verdict_t v = run_persona_with_channel(bullets, "imessage");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
}

/* WAY_TOO_LONG (>500 char for iMessage) — the persona stage's
 * shape-classifier gate must catch egregiously long content as
 * defense in depth even if the `shape` stage skipped it. The light
 * TOO_LONG penalty (-0.15) alone leaves score 0.85 = passed; this
 * test specifically exercises WAY_TOO_LONG (-0.3 + fatal flag). */
static void test_classifier_catches_way_too_long_for_imessage(void) {
    /* iMessage way_too_long threshold is 500. Build a 600-char body. */
    char *long_msg = malloc(601);
    HU_ASSERT_NOT_NULL(long_msg);
    memset(long_msg, 'a', 600);
    long_msg[0] = 'h';
    long_msg[1] = 'i';
    long_msg[2] = ' ';
    long_msg[600] = '\0';
    hu_outbound_verdict_t v = run_persona_with_channel(long_msg, "imessage");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
    free(long_msg);
}

/* ── Channel awareness ───────────────────────────────────────────── */

/* Slack relaxes markdown rules — bullet lists are OK there. A reply
 * that triggers REGENERATE on iMessage must SEND on Slack. This is
 * the load-bearing channel-awareness test: without it, the classifier
 * would over-trigger on technical contacts. */
static void test_classifier_respects_channel_relaxation_for_slack(void) {
    const char *slack_ok = "thanks! pushed the fix";
    hu_outbound_verdict_t v = run_persona_with_channel(slack_ok, "slack");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* NULL channel_name — defaults to iMessage strict rules per
 * hu_shape_channel_from_string. A clean iMessage-shape reply passes. */
static void test_classifier_null_channel_defaults_to_imessage(void) {
    hu_outbound_verdict_t v = run_persona_with_channel("yeah on my way", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* ── False-positive contracts (must NOT regenerate) ─────────────── */

/* Plain casual reply — corpus PASS class. Must SEND on any channel. */
static void test_classifier_passes_short_casual_reply(void) {
    hu_outbound_verdict_t v = run_persona_with_channel("haha yeah for sure", "imessage");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

static void test_classifier_passes_medium_casual_reply(void) {
    hu_outbound_verdict_t v = run_persona_with_channel(
        "totally — let me grab coffee first and then I'll head over", "imessage");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* Empty / NULL content — must not crash, must not REGENERATE
 * (treat as benign per existing persona_run convention). */
static void test_classifier_empty_content_sends(void) {
    hu_outbound_verdict_t v = run_persona_with_channel("", "imessage");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* ── Composition with existing heuristic ─────────────────────────── */

/* Project jargon STILL caught — the new classifier is added in
 * sequence, not replacing the old heuristic. corpus #11. */
static void test_existing_heuristic_still_fires_first(void) {
    hu_outbound_verdict_t v =
        run_persona_with_channel("finally got that Replay MCP stuff ready", "imessage");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
    HU_ASSERT_STR_EQ(v.reason, "persona_project_jargon");
}

void run_outbound_persona_classifier_tests(void) {
    HU_TEST_SUITE("outbound_persona_classifier");
    HU_RUN_TEST(test_classifier_catches_certainly_opener);
    HU_RUN_TEST(test_classifier_catches_great_question_opener);
    HU_RUN_TEST(test_classifier_catches_here_are_bullet_list);
    HU_RUN_TEST(test_classifier_catches_way_too_long_for_imessage);
    HU_RUN_TEST(test_classifier_respects_channel_relaxation_for_slack);
    HU_RUN_TEST(test_classifier_null_channel_defaults_to_imessage);
    HU_RUN_TEST(test_classifier_passes_short_casual_reply);
    HU_RUN_TEST(test_classifier_passes_medium_casual_reply);
    HU_RUN_TEST(test_classifier_empty_content_sends);
    HU_RUN_TEST(test_existing_heuristic_still_fires_first);
}
