/* DEF-15: the reactive path's spontaneous extras (double-text afterthought,
 * self-reaction, GIF) could never fire — they were gated on "did the reactive
 * path just send to this contact", which the reactive path records moments
 * before checking them. HU_SPONTANEITY makes them reachable and decides them
 * from Seth's learned rates scaled by the contact's bandit draw. */
#include "human/agent.h"
#include "human/agent/contextual_bandit.h"
#include "human/contact_send_recency.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/core/string.h"
#include "human/daemon/spontaneity.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char k_contact[] = "+15550003333";

static hu_spontaneity_rates_t rates_of(double dt, double sr, double gif) {
    hu_spontaneity_rates_t r;
    r.rate[HU_SPONT_DOUBLE_TEXT] = dt;
    r.rate[HU_SPONT_SELF_REACTION] = sr;
    r.rate[HU_SPONT_GIF] = gif;
    return r;
}

/* An agent whose reactive path JUST replied to the contact — exactly the state
 * the daemon is in when it reaches the extras. */
static hu_agent_t *agent_just_replied(void) {
    hu_agent_t *a = (hu_agent_t *)calloc(1, sizeof(*a));
    hu_contact_send_recency_record(&a->contact_send_recency, k_contact, strlen(k_contact),
                                   (int64_t)time(NULL), HU_SEND_PATH_REACTIVE);
    return a;
}

static hu_spontaneity_turn_t turn_for(hu_agent_t *a) {
    hu_spontaneity_turn_t t;
    memset(&t, 0, sizeof(t));
    t.agent = a;
    t.contact = k_contact;
    t.contact_len = strlen(k_contact);
    return t;
}

/* Headline: right after a reactive reply the legacy gate (OFF) can never open
 * — that was the bug — while LIVE opens it once per turn. */
static void spontaneity_live_reachable_after_reactive_reply(void) {
    hu_agent_t *a = agent_just_replied();
    hu_spontaneity_turn_t t = turn_for(a);
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;

    hu_spontaneity_set_for_test(HU_GATE_OFF, NULL, 1u);
    HU_ASSERT_FALSE(hu_daemon_spontaneity_gif_open(&t, now_ms)); /* the DEF-15 trap */

    hu_spontaneity_set_for_test(HU_GATE_LIVE, NULL, 1u);
    HU_ASSERT_TRUE(hu_daemon_spontaneity_gif_open(&t, now_ms));
    t.fired = 1; /* one extra already sent this turn */
    HU_ASSERT_FALSE(hu_daemon_spontaneity_gif_open(&t, now_ms));

    hu_spontaneity_set_for_test(-1, NULL, 0u);
    free(a);
}

/* SHADOW changes nothing that is sent: the legacy result is returned. */
static void spontaneity_shadow_keeps_legacy_result(void) {
    hu_agent_t *a = agent_just_replied();
    hu_spontaneity_turn_t t = turn_for(a);
    hu_spontaneity_rates_t r = rates_of(1.0, 1.0, 1.0);
    hu_spontaneity_set_for_test(HU_GATE_SHADOW, &r, 7u);
    HU_ASSERT_FALSE(hu_daemon_spontaneity_gif_open(&t, (uint64_t)time(NULL) * 1000ULL));
    HU_ASSERT_FALSE(hu_spontaneity_decide_for_test(&t, HU_SPONT_DOUBLE_TEXT, true));
    HU_ASSERT_EQ(t.fired, 0u);
    hu_spontaneity_set_for_test(-1, NULL, 0u);
    free(a);
}

/* LIVE without a learned rate never fires (shadow-only), even when eligible. */
static void spontaneity_live_without_learned_rate_never_fires(void) {
    hu_spontaneity_turn_t t = turn_for(NULL);
    hu_spontaneity_rates_t none = rates_of(-1.0, -1.0, -1.0);
    hu_spontaneity_set_for_test(HU_GATE_LIVE, &none, 3u);
    for (int i = 0; i < 200; i++)
        HU_ASSERT_FALSE(hu_spontaneity_decide_for_test(&t, HU_SPONT_GIF, true));
    HU_ASSERT_EQ(t.fired, 0u);
    hu_spontaneity_set_for_test(-1, NULL, 0u);
}

/* LIVE fires at Seth's measured rate, not at a constant: 0.30 and 0.05 give
 * clearly different frequencies; ineligible never fires; the per-turn cap
 * holds. */
static void spontaneity_live_fires_at_learned_rate(void) {
    hu_spontaneity_turn_t t = turn_for(NULL);
    hu_spontaneity_rates_t r = rates_of(0.30, 0.05, 0.0);
    hu_spontaneity_set_for_test(HU_GATE_LIVE, &r, 12345u);
    int dt = 0, sr = 0, gif = 0;
    for (int i = 0; i < 4000; i++) {
        t.fired = 0;
        dt += hu_spontaneity_decide_for_test(&t, HU_SPONT_DOUBLE_TEXT, true);
        t.fired = 0;
        sr += hu_spontaneity_decide_for_test(&t, HU_SPONT_SELF_REACTION, true);
        t.fired = 0;
        gif += hu_spontaneity_decide_for_test(&t, HU_SPONT_GIF, true);
    }
    HU_ASSERT_TRUE(dt > 1040 && dt < 1360); /* 0.30 * 4000 = 1200, ±4.4 sd */
    HU_ASSERT_TRUE(sr > 120 && sr < 280);   /* 0.05 * 4000 = 200 */
    HU_ASSERT_EQ(gif, 0);                   /* rate 0: Seth never sends GIFs */
    t.fired = 0;
    for (int i = 0; i < 50; i++)
        HU_ASSERT_FALSE(hu_spontaneity_decide_for_test(&t, HU_SPONT_DOUBLE_TEXT, false));
    hu_spontaneity_set_for_test(-1, NULL, 0u);
}

/* The contact's outcome history moves the rate within the trust region:
 * a contact who rewards expressiveness (Beta(20,2)) gets more extras than one
 * who ignores them (Beta(2,20)) at the same learned rate. */
static void spontaneity_bandit_theta_scales_rate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_bandit_t *bandit = NULL;
    HU_ASSERT_EQ(hu_contextual_bandit_create(&alloc, 16, &bandit), HU_OK);
    hu_agent_t *a = (hu_agent_t *)calloc(1, sizeof(*a));
    a->sota.bandit = bandit;
    hu_spontaneity_turn_t t = turn_for(a);
    uint64_t h = hu_contact_handle_hash(k_contact);

    hu_spontaneity_rates_t r = rates_of(0.30, 0.30, 0.30);
    hu_spontaneity_set_for_test(HU_GATE_LIVE, &r, 999u);
    HU_ASSERT_EQ(hu_contextual_bandit_set_arm(bandit, h, 20.0, 2.0, 20), HU_OK);
    int warm = 0;
    for (int i = 0; i < 4000; i++) {
        t.fired = 0;
        warm += hu_spontaneity_decide_for_test(&t, HU_SPONT_DOUBLE_TEXT, true);
    }
    HU_ASSERT_EQ(hu_contextual_bandit_set_arm(bandit, h, 2.0, 20.0, 20), HU_OK);
    int cold = 0;
    for (int i = 0; i < 4000; i++) {
        t.fired = 0;
        cold += hu_spontaneity_decide_for_test(&t, HU_SPONT_DOUBLE_TEXT, true);
    }
    /* warm p ~ 0.30*1.21 = 0.36 (1450), cold p ~ 0.30*0.84 = 0.25 (1010) */
    HU_ASSERT_TRUE(warm > cold + 250);
    HU_ASSERT_TRUE(warm < 4000 * 0.30 * 1.25 + 150); /* never past the trust region */
    HU_ASSERT_TRUE(cold > 4000 * 0.30 * 0.80 - 150);

    hu_spontaneity_set_for_test(-1, NULL, 0u);
    free(a);
    hu_contextual_bandit_destroy(bandit);
}

static void spontaneity_policy_contract(void) {
    hu_spontaneity_decision_t d = hu_spontaneity_policy(-1.0, 0.5, 0.0);
    HU_ASSERT_FALSE(d.fire);
    HU_ASSERT_FALSE(d.have_rate);
    d = hu_spontaneity_policy(0.2, -1.0, 0.19);
    HU_ASSERT_TRUE(d.fire);
    HU_ASSERT_TRUE(d.p > 0.1999 && d.p < 0.2001);
    d = hu_spontaneity_policy(0.2, -1.0, 0.21);
    HU_ASSERT_FALSE(d.fire);
    d = hu_spontaneity_policy(0.2, 1.0, 0.0);
    HU_ASSERT_TRUE(d.p > 0.2499 && d.p < 0.2501);
    d = hu_spontaneity_policy(0.2, 0.0, 0.0);
    HU_ASSERT_TRUE(d.p > 0.1599 && d.p < 0.1601);
    d = hu_spontaneity_policy(1.5, -1.0, 0.0); /* out of range = not measured */
    HU_ASSERT_FALSE(d.have_rate);
}

static void spontaneity_rates_parse_reads_optional_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_spontaneity_rates_t r;
    const char *j = "{\"double_text_rate\": 0.18, \"gif_rate\": 0.02, \"other\": 1}";
    HU_ASSERT_EQ(hu_spontaneity_rates_parse(&alloc, j, strlen(j), &r), HU_OK);
    HU_ASSERT_TRUE(r.rate[HU_SPONT_DOUBLE_TEXT] > 0.179 && r.rate[HU_SPONT_DOUBLE_TEXT] < 0.181);
    HU_ASSERT_TRUE(r.rate[HU_SPONT_GIF] > 0.019 && r.rate[HU_SPONT_GIF] < 0.021);
    HU_ASSERT_TRUE(r.rate[HU_SPONT_SELF_REACTION] < 0.0); /* absent */
    const char *bad = "{\"double_text_rate\": 3, \"gif_rate\": -0.5}";
    HU_ASSERT_EQ(hu_spontaneity_rates_parse(&alloc, bad, strlen(bad), &r), HU_OK);
    HU_ASSERT_TRUE(r.rate[HU_SPONT_DOUBLE_TEXT] < 0.0);
    HU_ASSERT_TRUE(r.rate[HU_SPONT_GIF] < 0.0);
    HU_ASSERT_EQ(hu_spontaneity_rates_parse(&alloc, "[1]", 3, &r), HU_ERR_PARSE);
}

void run_daemon_spontaneity_tests(void) {
    HU_TEST_SUITE("daemon_spontaneity");
    HU_RUN_TEST(spontaneity_live_reachable_after_reactive_reply);
    HU_RUN_TEST(spontaneity_shadow_keeps_legacy_result);
    HU_RUN_TEST(spontaneity_live_without_learned_rate_never_fires);
    HU_RUN_TEST(spontaneity_live_fires_at_learned_rate);
    HU_RUN_TEST(spontaneity_bandit_theta_scales_rate);
    HU_RUN_TEST(spontaneity_policy_contract);
    HU_RUN_TEST(spontaneity_rates_parse_reads_optional_fields);
}
