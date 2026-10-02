/* Reply-length policy (src/agent/turn/length_policy.c, HU_LENGTH_POLICY).
 *
 * Why (2026-10-01 humanness audit): the median reply was 22 chars against the
 * owner's ~71, and the cap came from a multiple of the INBOUND length with a
 * 15-char floor. The prompt then said "Keep it tight" for every cap <= 80, so
 * a contact whose measured p90 is 50 was told to keep it tight on every turn.
 * The policy caps from the owner's own per-contact reply distribution. */
#include "human/agent/ab_response.h"
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
     * p90/3 = 40, and the cap never drops below it. */
    hu_length_policy_input_t in = {
        .inbound_len = 2, .contact_p90 = 120, .legacy_cap = 15, .unbriefed_cap = 15};
    hu_length_policy_result_t r = hu_length_policy_compute(&in);
    HU_ASSERT_TRUE(r.from_stats);
    HU_ASSERT_TRUE(r.p50_derived);
    HU_ASSERT_EQ(r.p50, 40u);
    HU_ASSERT_EQ(r.cap, 40u);
    HU_ASSERT_TRUE(r.tight); /* short, casual inbound */
}

static void length_policy_brief_cap_never_pushes_below_p50(void) {
    hu_length_policy_input_t in = {.inbound_len = 2,
                                   .contact_p50 = 45,
                                   .contact_p90 = 120,
                                   .legacy_cap = 20,
                                   .unbriefed_cap = 120};
    hu_length_policy_result_t r = hu_length_policy_compute(&in);
    HU_ASSERT_FALSE(r.p50_derived);
    HU_ASSERT_EQ(r.cap, 45u);
}

static void length_policy_question_or_story_escapes_brief_cap(void) {
    /* Brief mode capped today's cap at 160; the unbriefed ratio cap is 200. */
    hu_length_policy_input_t in = {.inbound_len = 150,
                                   .shape = HU_LENGTH_SHAPE_STORY,
                                   .contact_p90 = 120,
                                   .legacy_cap = 160,
                                   .unbriefed_cap = 200};
    HU_ASSERT_EQ(hu_length_policy_compute(&in).cap, 200u);
    in.shape = HU_LENGTH_SHAPE_QUESTION;
    HU_ASSERT_EQ(hu_length_policy_compute(&in).cap, 200u);
    in.shape = 0; /* a plain statement stays at today's cap */
    HU_ASSERT_EQ(hu_length_policy_compute(&in).cap, 160u);
}

static void length_policy_never_below_todays_cap(void) {
    static const uint32_t legacy[] = {15, 50, 80, 99, 165, 200};
    for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
        for (unsigned shape = 0; shape < 4; shape++) {
            hu_length_policy_input_t in = {.inbound_len = 40,
                                           .shape = shape,
                                           .contact_p50 = 21,
                                           .contact_p90 = 50,
                                           .legacy_cap = legacy[i],
                                           .unbriefed_cap = legacy[i],
                                           .hard_max = 200};
            HU_ASSERT_GE(hu_length_policy_compute(&in).cap, legacy[i]);
        }
    }
}

static void length_policy_hard_bound_holds(void) {
    hu_length_policy_input_t in = {.inbound_len = 400,
                                   .shape = HU_LENGTH_SHAPE_STORY,
                                   .contact_p90 = 120,
                                   .legacy_cap = 160,
                                   .unbriefed_cap = 900,
                                   .hard_max = 200};
    HU_ASSERT_EQ(hu_length_policy_compute(&in).cap, 200u);
    in.hard_max = 0; /* no channel bound: the absolute ceiling still holds */
    HU_ASSERT_EQ(hu_length_policy_compute(&in).cap, HU_LENGTH_POLICY_HARD_MAX);

    /* The hard bound wins over the p50 floor. */
    hu_length_policy_input_t big = {.inbound_len = 300,
                                    .contact_p50 = 250,
                                    .contact_p90 = 400,
                                    .legacy_cap = 200,
                                    .unbriefed_cap = 200,
                                    .hard_max = 200};
    HU_ASSERT_EQ(hu_length_policy_compute(&big).cap, 200u);
}

static void length_policy_tight_describes_short_casual_inbound(void) {
    hu_length_policy_input_t in = {
        .inbound_len = 4, .contact_p50 = 21, .contact_p90 = 50, .legacy_cap = 50};
    HU_ASSERT_TRUE(hu_length_policy_compute(&in).tight); /* "Heyo" */
    in.shape = HU_LENGTH_SHAPE_QUESTION;
    HU_ASSERT_FALSE(hu_length_policy_compute(&in).tight); /* "u up?" */
    in.shape = 0;
    in.inbound_len = 39; /* casual but longer than the owner's own median */
    HU_ASSERT_FALSE(hu_length_policy_compute(&in).tight);
}

static void length_policy_missing_stats_returns_todays_formula(void) {
    /* No p50/p90: the cap is today's cap, verbatim, for every legacy value. */
    static const uint32_t legacy[] = {15, 50, 80, 81, 200, 300};
    for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
        hu_length_policy_input_t in = {.inbound_len = 300,
                                       .shape = HU_LENGTH_SHAPE_STORY,
                                       .legacy_cap = legacy[i],
                                       .unbriefed_cap = 300,
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

static void length_policy_turn_live_never_lowers_and_keeps_contact_multipliers(void) {
    /* A 39-char statement: today 39*2.55 = 99 > p90 50. LIVE keeps 99. */
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
    HU_ASSERT_EQ(live.cap, 99u);
    HU_ASSERT_EQ(live.tight, HU_LENGTH_TIGHT_NO);
    HU_ASSERT_TRUE(live.from_stats);

    /* A 60-char question from a friend at stage NEW: the friend multiplier
     * (2.75) gives 165, and LIVE must not drop it to a stage-only figure. */
    hu_contact_profile_t f = contact_with_p90(50);
    f.relationship_type = "friend";
    char q[61];
    memset(q, 'a', 59);
    q[59] = '?';
    q[60] = '\0';
    hu_length_turn_t tq = {
        .inbound = q, .inbound_len = 60, .contact = &f, .stage = HU_REL_NEW, .channel_max = 200};
    hu_length_policy_turn(&tq, HU_GATE_LIVE, &live);
    HU_ASSERT_GE(live.cap, 165u);

    /* Brief mode: a question escapes the brief cap in LIVE only. */
    hu_length_turn_t tb = tq;
    tb.inbound_len = 72;
    char qb[73];
    memset(qb, 'a', 71);
    qb[71] = '?';
    qb[72] = '\0';
    tb.inbound = qb;
    tb.brief_mode = true;
    tb.stage = HU_REL_FAMILIAR;
    hu_length_policy_turn(&tb, HU_GATE_OFF, &off);
    hu_length_policy_turn(&tb, HU_GATE_LIVE, &live);
    HU_ASSERT_EQ(off.cap, 160u);  /* FAMILIAR brief cap */
    HU_ASSERT_EQ(live.cap, 198u); /* 72 * 2.75 */

    /* Group turns and contacts without stats are never touched in LIVE. */
    t.is_group = true;
    t.contact = NULL;
    hu_length_policy_turn(&t, HU_GATE_OFF, &off);
    hu_length_policy_turn(&t, HU_GATE_LIVE, &live);
    HU_ASSERT_EQ(live.cap, off.cap);
    HU_ASSERT_EQ(live.tight, HU_LENGTH_TIGHT_LEGACY);
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

static void length_policy_prompt_live_tight_or_bare_limit(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t len = 0;
    char *p = build_limit_prompt(&a, 50, HU_LENGTH_TIGHT_NO, &len);
    HU_ASSERT_STR_NOT_CONTAINS(p, "Keep it tight");
    HU_ASSERT_STR_NOT_CONTAINS(p, "Stay within it");
    HU_ASSERT_STR_CONTAINS(p, "\nRESPONSE LIMIT: Maximum 50 characters.\n");
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

static char *render(hu_allocator_t *a, hu_persona_t *p, const char *incoming, size_t *len) {
    hu_reply_prompt_request_t req = {.persona = p,
                                     .channel = "imessage",
                                     .contact = "+15550001111",
                                     .incoming = incoming,
                                     .incoming_len = strlen(incoming),
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

    /* A question: today the cap (p90 50) is <= 80, so "Keep it tight". LIVE:
     * same cap, the bare limit, because a question is not short and casual. */
    size_t off_len = 0, live_len = 0, heyo_len = 0;
    hu_length_policy_set_mode_for_test(HU_GATE_OFF);
    char *off = render(&a, &p, "you around later?", &off_len);
    hu_length_policy_set_mode_for_test(HU_GATE_LIVE);
    char *live = render(&a, &p, "you around later?", &live_len);
    char *heyo = render(&a, &p, "Heyo", &heyo_len);
    hu_length_policy_set_mode_for_test(-1);

    HU_ASSERT_STR_CONTAINS(off, "RESPONSE LIMIT: Maximum 50 characters. Keep it tight.");
    HU_ASSERT_STR_NOT_CONTAINS(live, "Keep it tight");
    HU_ASSERT_STR_CONTAINS(live, "\nRESPONSE LIMIT: Maximum 50 characters.\n");
    /* "Heyo" is short and casual: LIVE still says keep it tight. */
    HU_ASSERT_STR_CONTAINS(heyo, "RESPONSE LIMIT: Maximum 50 characters. Keep it tight.");
    a.free(a.ctx, off, off_len + 1);
    a.free(a.ctx, live, live_len + 1);
    a.free(a.ctx, heyo, heyo_len + 1);
    hu_persona_deinit(&a, &p);
}

static void length_policy_off_render_is_byte_identical_to_unset(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_json(&a, k_persona_json, strlen(k_persona_json), &p), HU_OK);
    size_t l1 = 0, l2 = 0;
    hu_length_policy_set_mode_for_test(HU_GATE_OFF);
    char *a1 = render(&a, &p, "you around later?", &l1);
    hu_length_policy_set_mode_for_test(HU_GATE_SHADOW);
    char *a2 = render(&a, &p, "you around later?", &l2);
    hu_length_policy_set_mode_for_test(-1);
    HU_ASSERT_EQ(l1, l2);
    HU_ASSERT_EQ(memcmp(a1, a2, l1), 0);
    a.free(a.ctx, a1, l1 + 1);
    a.free(a.ctx, a2, l2 + 1);
    hu_persona_deinit(&a, &p);
}

static void length_policy_calibration_directive_uses_same_cap(void) {
    /* The calibration line carries its own "Target: ~N chars" number. Under
     * LIVE it must equal the RESPONSE LIMIT, channel bound included: the
     * relational formula says 99 (39 * 2.55, FAMILIAR), the channel 60. */
    hu_allocator_t a = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_json(&a, k_persona_json, strlen(k_persona_json), &p), HU_OK);
    const char *msg = "ok heading out now see you at the place";
    hu_reply_prompt_request_t req = {.persona = &p,
                                     .channel = "imessage",
                                     .contact = "+15550001111",
                                     .incoming = msg,
                                     .incoming_len = strlen(msg),
                                     .stage = HU_REL_FAMILIAR,
                                     .channel_max_chars = 60};
    char *off = NULL, *live = NULL;
    size_t off_len = 0, live_len = 0;
    hu_length_policy_set_mode_for_test(HU_GATE_OFF);
    HU_ASSERT_EQ(hu_reply_prompt_render(&a, &req, &off, &off_len), HU_OK);
    hu_length_policy_set_mode_for_test(HU_GATE_LIVE);
    HU_ASSERT_EQ(hu_reply_prompt_render(&a, &req, &live, &live_len), HU_OK);
    hu_length_policy_set_mode_for_test(-1);
    HU_ASSERT_STR_CONTAINS(off, "Target: ~99 chars max.");
    HU_ASSERT_STR_CONTAINS(off, "RESPONSE LIMIT: Maximum 60 characters");
    HU_ASSERT_STR_CONTAINS(live, "Target: ~60 chars max.");
    HU_ASSERT_STR_CONTAINS(live, "RESPONSE LIMIT: Maximum 60 characters");
    a.free(a.ctx, off, off_len + 1);
    a.free(a.ctx, live, live_len + 1);
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
    hu_quality_score_t off = hu_conversation_evaluate_quality_capped(reply, rl, h, 2, 70, true);
    hu_length_policy_set_mode_for_test(HU_GATE_LIVE);
    hu_quality_score_t live = hu_conversation_evaluate_quality_capped(reply, rl, h, 2, 70, true);
    hu_quality_score_t nostats =
        hu_conversation_evaluate_quality_capped(reply, rl, h, 2, 70, false);
    hu_quality_score_t plain = hu_conversation_evaluate_quality(reply, rl, h, 2, 70);
    hu_length_policy_set_mode_for_test(-1);

    HU_ASSERT_TRUE(off.needs_revision); /* today: "Tighten up significantly" */
    HU_ASSERT_STR_CONTAINS(off.guidance, "Tighten up");
    HU_ASSERT_FALSE(live.needs_revision);
    HU_ASSERT_GT(live.brevity, off.brevity);

    /* LIVE, but a group or a contact without stats: today's scoring exactly. */
    HU_ASSERT_TRUE(nostats.needs_revision);
    HU_ASSERT_EQ(nostats.brevity, off.brevity);
    HU_ASSERT_EQ(nostats.total, off.total);
    HU_ASSERT_EQ(plain.total, off.total);
    HU_ASSERT_TRUE(plain.needs_revision);

    /* LIVE still flags a reply far past the cap. */
    char big[400];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    hu_length_policy_set_mode_for_test(HU_GATE_LIVE);
    hu_quality_score_t over =
        hu_conversation_evaluate_quality_capped(big, strlen(big), h, 2, 70, true);
    hu_length_policy_set_mode_for_test(-1);
    HU_ASSERT_TRUE(over.needs_revision);
}

static void length_policy_quality_ref_is_unchanged_off(void) {
    HU_ASSERT_EQ(hu_length_policy_quality_over_ref(10, 70, HU_GATE_OFF, true), 10u);
    HU_ASSERT_EQ(hu_length_policy_quality_over_ref(10, 70, HU_GATE_SHADOW, true), 10u);
    HU_ASSERT_EQ(hu_length_policy_quality_over_ref(10, 70, HU_GATE_LIVE, true), 47u);
    HU_ASSERT_EQ(hu_length_policy_quality_over_ref(10, 70, HU_GATE_LIVE, false), 10u);
    HU_ASSERT_EQ(hu_length_policy_quality_over_ref(100, 70, HU_GATE_LIVE, true), 100u);
    HU_ASSERT_EQ(hu_length_policy_quality_over_ref(10, 0, HU_GATE_LIVE, true), 10u);
}

static void length_policy_ab_pick_scores_with_the_same_cap(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_channel_history_entry_t h[1];
    memset(h, 0, sizeof(h));
    snprintf(h[0].text, sizeof(h[0].text), "Heyo");
    hu_ab_result_t r = {.candidate_count = 1, .cap_from_stats = true};
    char reply[] = "hey! just got back from the gym, what are you up to tonight";
    r.candidates[0].response = reply;
    r.candidates[0].response_len = strlen(r.candidates[0].response);
    hu_length_policy_set_mode_for_test(HU_GATE_LIVE);
    HU_ASSERT_EQ(hu_ab_evaluate(&a, &r, h, 1, 70), HU_OK);
    int live_score = r.candidates[0].quality_score;
    r.cap_from_stats = false;
    HU_ASSERT_EQ(hu_ab_evaluate(&a, &r, h, 1, 70), HU_OK);
    hu_length_policy_set_mode_for_test(-1);
    HU_ASSERT_GT(live_score, r.candidates[0].quality_score);
}

void run_length_policy_tests(void) {
    HU_TEST_SUITE("length_policy");
    HU_RUN_TEST(length_policy_short_inbound_cap_at_least_p50);
    HU_RUN_TEST(length_policy_brief_cap_never_pushes_below_p50);
    HU_RUN_TEST(length_policy_question_or_story_escapes_brief_cap);
    HU_RUN_TEST(length_policy_never_below_todays_cap);
    HU_RUN_TEST(length_policy_hard_bound_holds);
    HU_RUN_TEST(length_policy_tight_describes_short_casual_inbound);
    HU_RUN_TEST(length_policy_missing_stats_returns_todays_formula);
    HU_RUN_TEST(length_policy_inbound_shape_flags);
    HU_RUN_TEST(length_policy_turn_off_is_todays_cap_exactly);
    HU_RUN_TEST(length_policy_turn_live_never_lowers_and_keeps_contact_multipliers);
    HU_RUN_TEST(length_policy_calibration_directive_uses_same_cap);
    HU_RUN_TEST(length_policy_prompt_legacy_line_is_byte_identical);
    HU_RUN_TEST(length_policy_prompt_live_tight_or_bare_limit);
    HU_RUN_TEST(length_policy_persona_parses_p50);
    HU_RUN_TEST(length_policy_live_changes_response_limit_line);
    HU_RUN_TEST(length_policy_off_render_is_byte_identical_to_unset);
    HU_RUN_TEST(length_policy_quality_retry_does_not_fight_live_cap);
    HU_RUN_TEST(length_policy_quality_ref_is_unchanged_off);
    HU_RUN_TEST(length_policy_ab_pick_scores_with_the_same_cap);
}
