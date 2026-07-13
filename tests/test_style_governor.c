/* Tests for the style-governor outbound stage.
 *
 * Targets come from the MEASURED style card (scripts/persona_style_card.py,
 * 2026-07-12, n=1488 typed messages): 79% of the persona's real texts end
 * with no terminal punctuation, only 9% end with '?'. The model baseline is
 * 10% / 31% — terminal punctuation and the reciprocal trailing question are
 * the two strongest "this is AI" tells. */
#include "human/agent/outbound_pipeline.h"
#include "human/agent/style_governor.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_style_governor;

static hu_allocator_t test_alloc;

static void shape_expect(const char *in, unsigned roll, const char *want, unsigned want_actions) {
    test_alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    unsigned actions = 0;
    HU_ASSERT_EQ(
        hu_style_governor_shape(&test_alloc, in, strlen(in), roll, &out, &out_len, &actions),
        HU_OK);
    if (want == NULL) {
        HU_ASSERT_NULL(out);
        HU_ASSERT_EQ(actions, 0u);
    } else {
        HU_ASSERT_NOT_NULL(out);
        HU_ASSERT_STR_EQ(out, want);
        HU_ASSERT_EQ(actions, want_actions);
        test_alloc.free(test_alloc.ctx, out, out_len + 1);
    }
}

/* ── Action A: terminal period ─────────────────────────────────────── */

static void shape_strips_single_terminal_period(void) {
    shape_expect("Sounds good.", 0, "Sounds good", HU_STYLE_GOV_ACTION_PERIOD_STRIPPED);
}

static void shape_keeps_period_when_roll_above_gate(void) {
    /* roll 95 >= 90 → this message keeps its period (the ~10% that do). */
    shape_expect("Sounds good.", 95, NULL, 0);
}

static void shape_preserves_ellipsis_and_other_terminals(void) {
    shape_expect("I gave up\xE2\x80\xA6", 0, NULL, 0); /* U+2026 */
    shape_expect("ok...", 0, NULL, 0);                 /* ascii ellipsis */
    shape_expect("Really?!", 0, NULL, 0);
    shape_expect("nice", 0, NULL, 0); /* already bare */
}

static void shape_period_strip_is_deterministic_per_text(void) {
    /* Same text → same roll → same outcome, run twice. */
    const char *t = "On my way.";
    unsigned r1 = hu_style_governor_roll(t, strlen(t));
    unsigned r2 = hu_style_governor_roll(t, strlen(t));
    HU_ASSERT_EQ(r1, r2);
    HU_ASSERT_TRUE(r1 < 100);
}

/* ── Action B: reciprocal trailing question ───────────────────────── */

static void shape_strips_reciprocal_trailing_question(void) {
    shape_expect("Not much, just hanging out. What's up with you?", 95,
                 "Not much, just hanging out.", HU_STYLE_GOV_ACTION_QUESTION_STRIPPED);
}

static void shape_strips_how_about_you_variant(void) {
    shape_expect("Long day, glad it's over. How about you?", 95, "Long day, glad it's over.",
                 HU_STYLE_GOV_ACTION_QUESTION_STRIPPED);
}

static void shape_keeps_genuine_content_question(void) {
    /* Content-bearing questions are NOT boilerplate — exact-phrase match
     * only (substring-classifier-pitfalls: "how was your day with the kids"
     * carries content and must survive). */
    shape_expect("Wait, a beach house? When was that?", 95, NULL, 0);
    shape_expect("How was your day with the kids?", 95, NULL, 0);
}

static void shape_keeps_boilerplate_when_whole_message(void) {
    /* Nothing left if stripped — leave it alone. */
    shape_expect("how about you?", 95, NULL, 0);
}

static void shape_applies_both_actions_together(void) {
    /* Question stripped first leaves a terminal period; roll 0 strips it. */
    shape_expect("Not much, just chilling. What about you?", 0, "Not much, just chilling",
                 HU_STYLE_GOV_ACTION_PERIOD_STRIPPED | HU_STYLE_GOV_ACTION_QUESTION_STRIPPED);
}

/* ── Stage mode gating ────────────────────────────────────────────── */

static hu_outbound_verdict_t run_stage(const char *text, char **replacement_out) {
    test_alloc = hu_system_allocator();
    hu_outbound_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.content = (char *)text;
    msg.content_len = strlen(text);
    hu_outbound_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.alloc = &test_alloc;
    ctx.channel_name = "imessage";
    ctx.path = HU_OUTBOUND_PATH_REACTIVE;
    hu_outbound_verdict_t v = hu_outbound_pipeline_stage_style_governor.run(
        &hu_outbound_pipeline_stage_style_governor, &msg, &ctx);
    if (replacement_out)
        *replacement_out = v.replacement;
    return v;
}

static void stage_off_passes_through(void) {
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_OFF);
    hu_outbound_verdict_t v = run_stage("Sounds good.", NULL);
    HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_SEND);
    hu_style_governor_set_mode_for_test(-1);
}

static void stage_shadow_passes_through_unchanged(void) {
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_SHADOW);
    hu_outbound_verdict_t v = run_stage("Sounds good.", NULL);
    HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_SEND);
    HU_ASSERT_NULL(v.replacement);
    hu_style_governor_set_mode_for_test(-1);
}

static void stage_live_rewrites_when_shaping_applies(void) {
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_LIVE);
    char *replacement = NULL;
    /* "Sounds good." rolls below the gate for this fixture text only if
     * its hash says so — pick a text we KNOW rolls under 90 by querying
     * the roll first and asserting on the matching expectation. */
    const char *t = "Sounds good.";
    unsigned roll = hu_style_governor_roll(t, strlen(t));
    hu_outbound_verdict_t v = run_stage(t, &replacement);
    if (roll < HU_STYLE_GOV_PERIOD_STRIP_PCT) {
        HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_REWRITE);
        HU_ASSERT_NOT_NULL(replacement);
        HU_ASSERT_STR_EQ(replacement, "Sounds good");
        test_alloc.free(test_alloc.ctx, replacement, v.replacement_len + 1);
    } else {
        HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_SEND);
    }
    hu_style_governor_set_mode_for_test(-1);
}

static void stage_live_sends_when_nothing_to_shape(void) {
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_LIVE);
    hu_outbound_verdict_t v = run_stage("nice", NULL);
    HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_SEND);
    HU_ASSERT_NULL(v.replacement);
    hu_style_governor_set_mode_for_test(-1);
}

/* Reactive-path in-place apply — the daemon send path bypasses the outbound
 * pipeline (2026-07-12 egress audit), so this helper carries the same gate +
 * shaping to normal replies. OFF/SHADOW never mutate; LIVE shapes in place. */
static void apply_inplace_off_is_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_OFF);
    char buf[64] = "Sounds good.";
    size_t out = hu_style_governor_apply_inplace(&alloc, buf, strlen(buf));
    HU_ASSERT_EQ(out, (size_t)12);
    HU_ASSERT_STR_EQ(buf, "Sounds good.");
    hu_style_governor_set_mode_for_test(-1);
}

static void apply_inplace_shadow_is_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_SHADOW);
    char buf[64] = "Sounds good.";
    size_t out = hu_style_governor_apply_inplace(&alloc, buf, strlen(buf));
    HU_ASSERT_EQ(out, (size_t)12);
    HU_ASSERT_STR_EQ(buf, "Sounds good.");
    hu_style_governor_set_mode_for_test(-1);
}

static void apply_inplace_live_shapes_terminal_period(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_LIVE);
    /* "Sounds good." rolls under the 90 gate (deterministic hash), so LIVE
     * strips the terminal period in place. */
    char buf[64] = "Sounds good.";
    unsigned roll = hu_style_governor_roll(buf, strlen(buf));
    size_t out = hu_style_governor_apply_inplace(&alloc, buf, strlen(buf));
    if (roll < HU_STYLE_GOV_PERIOD_STRIP_PCT) {
        HU_ASSERT_EQ(out, (size_t)11);
        HU_ASSERT_STR_EQ(buf, "Sounds good");
    } else {
        HU_ASSERT_EQ(out, (size_t)12);
    }
    hu_style_governor_set_mode_for_test(-1);
}

static void apply_inplace_live_strips_reciprocal_question(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_LIVE);
    char buf[80] = "Not much, just hanging out. What's up with you?";
    unsigned roll = hu_style_governor_roll(buf, strlen(buf));
    size_t out = hu_style_governor_apply_inplace(&alloc, buf, strlen(buf));
    /* The reciprocal question is always stripped; the exposed terminal
     * period is then also stripped iff the roll is under the gate. Either
     * way the "what's up with you" boilerplate is gone. */
    if (roll < HU_STYLE_GOV_PERIOD_STRIP_PCT) {
        HU_ASSERT_STR_EQ(buf, "Not much, just hanging out");
        HU_ASSERT_EQ(out, (size_t)26);
    } else {
        HU_ASSERT_STR_EQ(buf, "Not much, just hanging out.");
        HU_ASSERT_EQ(out, (size_t)27);
    }
    HU_ASSERT_TRUE(strstr(buf, "you") == NULL);
    hu_style_governor_set_mode_for_test(-1);
}

void run_style_governor_tests(void) {
    HU_TEST_SUITE("style_governor");
    HU_RUN_TEST(apply_inplace_off_is_noop);
    HU_RUN_TEST(apply_inplace_shadow_is_noop);
    HU_RUN_TEST(apply_inplace_live_shapes_terminal_period);
    HU_RUN_TEST(apply_inplace_live_strips_reciprocal_question);
    HU_RUN_TEST(shape_strips_single_terminal_period);
    HU_RUN_TEST(shape_keeps_period_when_roll_above_gate);
    HU_RUN_TEST(shape_preserves_ellipsis_and_other_terminals);
    HU_RUN_TEST(shape_period_strip_is_deterministic_per_text);
    HU_RUN_TEST(shape_strips_reciprocal_trailing_question);
    HU_RUN_TEST(shape_strips_how_about_you_variant);
    HU_RUN_TEST(shape_keeps_genuine_content_question);
    HU_RUN_TEST(shape_keeps_boilerplate_when_whole_message);
    HU_RUN_TEST(shape_applies_both_actions_together);
    HU_RUN_TEST(stage_off_passes_through);
    HU_RUN_TEST(stage_shadow_passes_through_unchanged);
    HU_RUN_TEST(stage_live_rewrites_when_shaping_applies);
    HU_RUN_TEST(stage_live_sends_when_nothing_to_shape);
}
