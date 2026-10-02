/* Reply-length policy (src/agent/turn/length_policy.c, HU_LENGTH_POLICY).
 *
 * Why (2026-10-01 humanness audit): the median reply was 22 chars against the
 * owner's ~71, and the cap came from a multiple of the INBOUND length with a
 * 15-char floor. The prompt then said "Keep it tight" for every cap <= 80, so
 * a contact whose measured p90 is 50 was told to keep it tight on every turn.
 * The policy caps from the owner's own per-contact reply distribution. */
#include "human/agent/length_policy.h"
#include "human/agent/prompt.h"
#include "human/agent/reply_prompt.h"
#include "human/channel.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── pure policy ─────────────────────────────────────────────────────── */

static void length_policy_short_inbound_cap_at_least_p50(void) {
    /* "ok" to a contact with p90=120 and no measured p50: the derived p50 is
     * p90/3 = 40, and the cap is the owner's p90, never the 2x-inbound 15. */
    hu_length_policy_input_t in = {
        .inbound_len = 2, .contact_p90 = 120, .stage = HU_REL_NEW, .legacy_cap = 15};
    hu_length_policy_result_t r = hu_length_policy_compute(&in);
    HU_ASSERT_TRUE(r.from_stats);
    HU_ASSERT_TRUE(r.p50_derived);
    HU_ASSERT_EQ(r.p50, 40u);
    HU_ASSERT_GE(r.cap, r.p50);
    HU_ASSERT_EQ(r.cap, 120u);
    HU_ASSERT_FALSE(r.tight);
}

static void length_policy_brief_cap_never_pushes_below_p50(void) {
    hu_length_policy_input_t in = {.inbound_len = 2,
                                   .contact_p50 = 45,
                                   .contact_p90 = 120,
                                   .stage = HU_REL_NEW,
                                   .legacy_cap = 15,
                                   .brief_cap = 20};
    hu_length_policy_result_t r = hu_length_policy_compute(&in);
    HU_ASSERT_FALSE(r.p50_derived);
    HU_ASSERT_EQ(r.cap, 45u);
    HU_ASSERT_FALSE(r.tight);
}

static void length_policy_long_story_inbound_raises_cap(void) {
    hu_length_policy_input_t in = {.inbound_len = 150,
                                   .shape = HU_LENGTH_SHAPE_STORY,
                                   .contact_p90 = 120,
                                   .stage = HU_REL_DEEP,
                                   .legacy_cap = 300};
    hu_length_policy_result_t r = hu_length_policy_compute(&in);
    /* 150 * 3.25 (the deep-relationship multiplier) = 487 > p90 120. */
    HU_ASSERT_EQ(r.cap, 487u);

    /* The same length without a shape flag still counts as long (>= 120). */
    in.shape = 0;
    HU_ASSERT_EQ(hu_length_policy_compute(&in).cap, 487u);

    /* A short, unshaped inbound does not raise it. */
    in.inbound_len = 40;
    HU_ASSERT_EQ(hu_length_policy_compute(&in).cap, 120u);

    /* A short QUESTION does (40 * 3.25 = 130). */
    in.shape = HU_LENGTH_SHAPE_QUESTION;
    HU_ASSERT_EQ(hu_length_policy_compute(&in).cap, 130u);
}

static void length_policy_hard_bound_holds(void) {
    hu_length_policy_input_t in = {.inbound_len = 400,
                                   .shape = HU_LENGTH_SHAPE_STORY,
                                   .contact_p90 = 120,
                                   .stage = HU_REL_DEEP,
                                   .legacy_cap = 200,
                                   .hard_max = 200};
    HU_ASSERT_EQ(hu_length_policy_compute(&in).cap, 200u);

    /* No channel bound: the absolute ceiling still holds. */
    in.hard_max = 0;
    HU_ASSERT_EQ(hu_length_policy_compute(&in).cap, HU_LENGTH_POLICY_HARD_MAX);

    /* The hard bound wins over the p50 floor — and then the cap IS below the
     * owner's p50, so the prompt may say "keep it tight". */
    hu_length_policy_input_t big = {.inbound_len = 2,
                                    .contact_p50 = 250,
                                    .contact_p90 = 400,
                                    .stage = HU_REL_NEW,
                                    .legacy_cap = 200,
                                    .hard_max = 200};
    hu_length_policy_result_t r = hu_length_policy_compute(&big);
    HU_ASSERT_EQ(r.cap, 200u);
    HU_ASSERT_TRUE(r.tight);
}

static void length_policy_missing_stats_returns_todays_formula(void) {
    /* No p50/p90: the cap is today's cap, verbatim, for every legacy value. */
    static const uint32_t legacy[] = {15, 50, 80, 81, 200, 300};
    for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
        hu_length_policy_input_t in = {.inbound_len = 300,
                                       .shape = HU_LENGTH_SHAPE_STORY,
                                       .stage = HU_REL_DEEP,
                                       .legacy_cap = legacy[i],
                                       .hard_max = 200};
        hu_length_policy_result_t r = hu_length_policy_compute(&in);
        HU_ASSERT_FALSE(r.from_stats);
        HU_ASSERT_EQ(r.cap, legacy[i]);
        HU_ASSERT_EQ(r.tight, hu_length_policy_legacy_tight(legacy[i]));
    }
}

static void length_policy_inbound_shape_flags(void) {
    HU_ASSERT_EQ(hu_length_policy_inbound_shape("lol", 3), 0u);
    HU_ASSERT_EQ(hu_length_policy_inbound_shape("you free tonight?", 17),
                 (unsigned)HU_LENGTH_SHAPE_QUESTION);
    const char *story = "So I finally went to the doctor today. They said it is nothing "
                        "serious. But I have to go back next week for more tests.";
    HU_ASSERT_TRUE(hu_length_policy_inbound_shape(story, strlen(story)) & HU_LENGTH_SHAPE_STORY);
    HU_ASSERT_EQ(hu_length_policy_inbound_shape(NULL, 0), 0u);
}

/* ── turn glue: legacy computation moved out of daemon.c ──────────────── */

static hu_contact_profile_t contact_with_p90(uint16_t p90) {
    hu_contact_profile_t c;
    memset(&c, 0, sizeof(c));
    c.reply_chars_p90 = p90;
    return c;
}

static void length_policy_turn_off_is_todays_cap_exactly(void) {
    hu_contact_profile_t c = contact_with_p90(50);
    const char *in[] = {"Heyo", "you around later?",
                        ("ok so here is the thing about today and "
                         "why I could not make it to the game")};
    for (size_t i = 0; i < 3; i++) {
        size_t n = strlen(in[i]);
        for (int brief = 0; brief < 2; brief++) {
            for (int group = 0; group < 2; group++) {
                hu_length_turn_t t = {.inbound = in[i],
                                      .inbound_len = n,
                                      .contact = group ? NULL : &c,
                                      .stage = HU_REL_FAMILIAR,
                                      .channel_max = 200,
                                      .is_group = group != 0,
                                      .brief_mode = brief != 0};
                /* The daemon.c F15 + brief block as it was before the move. */
                uint32_t want = 200;
                int cal =
                    group ? hu_conversation_max_response_chars(n)
                          : hu_conversation_max_response_chars_relational(n, &c, HU_REL_FAMILIAR);
                if (cal > 0 && (uint32_t)cal < want)
                    want = (uint32_t)cal;
                if (brief) {
                    uint32_t bc = hu_conversation_brief_char_cap(group != 0, group ? NULL : &c,
                                                                 HU_REL_FAMILIAR);
                    if (want > bc)
                        want = bc;
                }
                hu_length_turn_result_t r;
                hu_length_policy_turn(&t, HU_GATE_OFF, &r);
                HU_ASSERT_EQ(r.cap, want);
                HU_ASSERT_EQ(r.tight, HU_LENGTH_TIGHT_LEGACY);
                /* SHADOW computes the new cap but applies the old one. */
                hu_length_policy_turn(&t, HU_GATE_SHADOW, &r);
                HU_ASSERT_EQ(r.cap, want);
                HU_ASSERT_EQ(r.tight, HU_LENGTH_TIGHT_LEGACY);
                HU_ASSERT_EQ(r.old_cap, want);
            }
        }
    }
}

static void length_policy_turn_live_applies_new_cap(void) {
    /* p90 50 → derived p50 16. Today: cap 50, "keep it tight" (<= 80).
     * LIVE: cap 50, tight=NO because 50 >= p50. A 40-char statement today
     * gets 40*2.55 = 102; LIVE caps it at the owner's p90 (50). */
    hu_contact_profile_t c = contact_with_p90(50);
    const char *msg = "ok heading out now see you at the place";
    hu_length_turn_t t = {.inbound = msg,
                          .inbound_len = strlen(msg),
                          .contact = &c,
                          .stage = HU_REL_FAMILIAR,
                          .channel_max = 200};
    hu_length_turn_result_t off, live;
    hu_length_policy_turn(&t, HU_GATE_OFF, &off);
    hu_length_policy_turn(&t, HU_GATE_LIVE, &live);
    HU_ASSERT_EQ(off.cap, 99u);
    HU_ASSERT_EQ(live.cap, 50u);
    HU_ASSERT_EQ(live.tight, HU_LENGTH_TIGHT_NO);
    HU_ASSERT_TRUE(live.from_stats);

    /* Group turns and contacts without stats are never touched in LIVE. */
    t.is_group = true;
    t.contact = NULL;
    hu_length_policy_turn(&t, HU_GATE_OFF, &off);
    hu_length_policy_turn(&t, HU_GATE_LIVE, &live);
    HU_ASSERT_EQ(live.cap, off.cap);
    HU_ASSERT_EQ(live.tight, HU_LENGTH_TIGHT_LEGACY);
}

static void length_policy_calibration_directive_uses_same_cap(void) {
    /* The conversation-context calibration line carries its own "Target: ~N
     * chars" number; under LIVE it must match the RESPONSE LIMIT cap. */
    hu_contact_profile_t c = contact_with_p90(50);
    const char *msg = "ok heading out now see you at the place";
    char off[1024], live[1024];
    hu_length_policy_set_mode_for_test(HU_GATE_OFF);
    size_t n_off = hu_conversation_calibrate_length_for_contact(
        msg, strlen(msg), NULL, 0, false, &c, HU_REL_FAMILIAR, off, sizeof(off));
    hu_length_policy_set_mode_for_test(HU_GATE_LIVE);
    size_t n_live = hu_conversation_calibrate_length_for_contact(
        msg, strlen(msg), NULL, 0, false, &c, HU_REL_FAMILIAR, live, sizeof(live));
    hu_length_policy_set_mode_for_test(-1);
    HU_ASSERT_GT(n_off, 0u);
    HU_ASSERT_GT(n_live, 0u);
    HU_ASSERT_STR_CONTAINS(off, "Target: ~99 chars max.");
    HU_ASSERT_STR_CONTAINS(live, "Target: ~50 chars max.");
}

/* ── prompt line ──────────────────────────────────────────────────────── */

static char *build_limit_prompt(hu_allocator_t *a, uint32_t cap, hu_length_tight_t tight,
                                size_t *len) {
    hu_prompt_config_t cfg = {.persona_prompt = "You are Seth.",
                              .persona_prompt_len = 13,
                              .persona_immersive = true,
                              .max_response_chars = cap,
                              .response_limit_tight = tight};
    char *out = NULL;
    HU_ASSERT_EQ(hu_prompt_build_system(a, &cfg, NULL, NULL, &out, len), HU_OK);
    return out;
}

static void length_policy_prompt_legacy_line_is_byte_identical(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t len = 0;
    char *p = build_limit_prompt(&a, 50, HU_LENGTH_TIGHT_LEGACY, &len);
    HU_ASSERT_STR_CONTAINS(p, "\nRESPONSE LIMIT: Maximum 50 characters. Keep it tight.\n");
    a.free(a.ctx, p, len + 1);
    p = build_limit_prompt(&a, 120, HU_LENGTH_TIGHT_LEGACY, &len);
    HU_ASSERT_STR_CONTAINS(p, "\nRESPONSE LIMIT: Maximum 120 characters. Stay within it, but sound "
                              "like a real text thread — natural wording beats robotic "
                              "truncation.\n");
    a.free(a.ctx, p, len + 1);
}

static void length_policy_prompt_live_says_tight_only_below_p50(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t len = 0;
    char *p = build_limit_prompt(&a, 50, HU_LENGTH_TIGHT_NO, &len);
    HU_ASSERT_STR_NOT_CONTAINS(p, "Keep it tight");
    HU_ASSERT_STR_CONTAINS(p, "RESPONSE LIMIT: Maximum 50 characters. Stay within it");
    a.free(a.ctx, p, len + 1);
    p = build_limit_prompt(&a, 200, HU_LENGTH_TIGHT_YES, &len);
    HU_ASSERT_STR_CONTAINS(p, "RESPONSE LIMIT: Maximum 200 characters. Keep it tight.");
    a.free(a.ctx, p, len + 1);
}

/* ── end to end through the production prompt renderer ────────────────── */

static const char k_persona_json[] =
    "{\"version\":1,\"name\":\"lptest\","
    "\"core\":{\"identity\":\"Seth\",\"traits\":[\"warm\"]},"
    "\"contacts\":{\"+15550001111\":{\"name\":\"Lexi\",\"relationship\":\"friend\","
    "\"reply_chars_p90\":50,\"reply_chars_p50\":21}}}";

static char *render(hu_allocator_t *a, hu_persona_t *p, size_t *len) {
    hu_reply_prompt_request_t req = {.persona = p,
                                     .channel = "imessage",
                                     .contact = "+15550001111",
                                     .incoming = "Heyo",
                                     .incoming_len = 4,
                                     .stage = HU_REL_NEW,
                                     .channel_max_chars = 200};
    char *out = NULL;
    HU_ASSERT_EQ(hu_reply_prompt_render(a, &req, &out, len), HU_OK);
    return out;
}

static void length_policy_persona_parses_p50(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_json(&a, k_persona_json, strlen(k_persona_json), &p), HU_OK);
    const hu_contact_profile_t *c = hu_persona_find_contact(&p, "+15550001111", 12);
    HU_ASSERT_NOT_NULL(c);
    HU_ASSERT_EQ(c->reply_chars_p50, 21u);
    HU_ASSERT_EQ(c->reply_chars_p90, 50u);
    hu_persona_deinit(&a, &p);
}

static void length_policy_live_changes_response_limit_line(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_json(&a, k_persona_json, strlen(k_persona_json), &p), HU_OK);

    size_t off_len = 0, live_len = 0;
    hu_length_policy_set_mode_for_test(HU_GATE_OFF);
    char *off = render(&a, &p, &off_len);
    hu_length_policy_set_mode_for_test(HU_GATE_LIVE);
    char *live = render(&a, &p, &live_len);
    hu_length_policy_set_mode_for_test(-1);

    /* OFF: today's line. LIVE: same cap (p90 50 >= p50 21) but not "tight". */
    HU_ASSERT_STR_CONTAINS(off, "RESPONSE LIMIT: Maximum 50 characters. Keep it tight.");
    HU_ASSERT_STR_NOT_CONTAINS(live, "Keep it tight");
    HU_ASSERT_STR_CONTAINS(live, "RESPONSE LIMIT: Maximum 50 characters. Stay within it");
    a.free(a.ctx, off, off_len + 1);
    a.free(a.ctx, live, live_len + 1);
    hu_persona_deinit(&a, &p);
}

static void length_policy_off_render_is_byte_identical_to_unset(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_json(&a, k_persona_json, strlen(k_persona_json), &p), HU_OK);
    size_t l1 = 0, l2 = 0;
    hu_length_policy_set_mode_for_test(HU_GATE_OFF);
    char *a1 = render(&a, &p, &l1);
    hu_length_policy_set_mode_for_test(HU_GATE_SHADOW);
    char *a2 = render(&a, &p, &l2);
    hu_length_policy_set_mode_for_test(-1);
    HU_ASSERT_EQ(l1, l2);
    HU_ASSERT_EQ(memcmp(a1, a2, l1), 0);
    a.free(a.ctx, a1, l1 + 1);
    a.free(a.ctx, a2, l2 + 1);
    hu_persona_deinit(&a, &p);
}

/* ── quality re-generation scores against the same cap ────────────────── */

static void length_policy_quality_retry_does_not_fight_live_cap(void) {
    hu_channel_history_entry_t h[2];
    memset(h, 0, sizeof(h));
    h[0].from_me = true;
    snprintf(h[0].text, sizeof(h[0].text), "hey you");
    h[1].from_me = false;
    snprintf(h[1].text, sizeof(h[1].text), "Heyo");
    /* 60 chars, inside a cap of 70; 6x their 4-char (floored 10) message. */
    const char *reply = "hey! just got back from the gym, what are you up to tonight";
    size_t rl = strlen(reply);

    hu_length_policy_set_mode_for_test(HU_GATE_OFF);
    hu_quality_score_t off = hu_conversation_evaluate_quality(reply, rl, h, 2, 70);
    hu_length_policy_set_mode_for_test(HU_GATE_LIVE);
    hu_quality_score_t live = hu_conversation_evaluate_quality(reply, rl, h, 2, 70);
    hu_length_policy_set_mode_for_test(-1);

    HU_ASSERT_TRUE(off.needs_revision); /* today: "Tighten up significantly" */
    HU_ASSERT_STR_CONTAINS(off.guidance, "Tighten up");
    HU_ASSERT_FALSE(live.needs_revision);
    HU_ASSERT_GT(live.brevity, off.brevity);

    /* LIVE still flags a reply far past the cap. */
    char big[400];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    hu_length_policy_set_mode_for_test(HU_GATE_LIVE);
    hu_quality_score_t over = hu_conversation_evaluate_quality(big, strlen(big), h, 2, 70);
    hu_length_policy_set_mode_for_test(-1);
    HU_ASSERT_TRUE(over.needs_revision);
}

static void length_policy_quality_ref_is_unchanged_off(void) {
    HU_ASSERT_EQ(hu_length_policy_quality_over_ref(10, 70, HU_GATE_OFF), 10u);
    HU_ASSERT_EQ(hu_length_policy_quality_over_ref(10, 70, HU_GATE_SHADOW), 10u);
    HU_ASSERT_EQ(hu_length_policy_quality_over_ref(10, 70, HU_GATE_LIVE), 47u);
    HU_ASSERT_EQ(hu_length_policy_quality_over_ref(100, 70, HU_GATE_LIVE), 100u);
    HU_ASSERT_EQ(hu_length_policy_quality_over_ref(10, 0, HU_GATE_LIVE), 10u);
}

void run_length_policy_tests(void) {
    HU_TEST_SUITE("length_policy");
    HU_RUN_TEST(length_policy_short_inbound_cap_at_least_p50);
    HU_RUN_TEST(length_policy_brief_cap_never_pushes_below_p50);
    HU_RUN_TEST(length_policy_long_story_inbound_raises_cap);
    HU_RUN_TEST(length_policy_hard_bound_holds);
    HU_RUN_TEST(length_policy_missing_stats_returns_todays_formula);
    HU_RUN_TEST(length_policy_inbound_shape_flags);
    HU_RUN_TEST(length_policy_turn_off_is_todays_cap_exactly);
    HU_RUN_TEST(length_policy_turn_live_applies_new_cap);
    HU_RUN_TEST(length_policy_calibration_directive_uses_same_cap);
    HU_RUN_TEST(length_policy_prompt_legacy_line_is_byte_identical);
    HU_RUN_TEST(length_policy_prompt_live_says_tight_only_below_p50);
    HU_RUN_TEST(length_policy_persona_parses_p50);
    HU_RUN_TEST(length_policy_live_changes_response_limit_line);
    HU_RUN_TEST(length_policy_off_render_is_byte_identical_to_unset);
    HU_RUN_TEST(length_policy_quality_retry_does_not_fight_live_cap);
    HU_RUN_TEST(length_policy_quality_ref_is_unchanged_off);
}
