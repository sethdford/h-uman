/* DEF-15: the reactive path's spontaneous extras (double-text afterthought,
 * self-reaction, GIF) could never fire — they were gated on "did the reactive
 * path just send to this contact", which the reactive path records moments
 * before checking them. HU_SPONTANEITY makes them reachable, decides them
 * from a learned per-(contact, kind) Beta posterior updated by outcomes,
 * allows one extra per turn, and never sleeps on the daemon loop. */
#include "human/agent.h"
#include "human/contact_send_recency.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/daemon/spontaneity.h"
#include "human/persona.h"
#include "test_framework.h"
#include "test_tmpdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char k_contact[] = "+15550003333";

/* ── mock channel + provider ────────────────────────────────────────────── */

static int s_sends, s_media_sends, s_reacts, s_llm_calls;
static char s_last_text[256];
static char s_last_media[512];

static const char *mock_name(void *ctx) {
    (void)ctx;
    return "imessage";
}
static hu_error_t mock_send(void *ctx, const char *target, size_t target_len, const char *msg,
                            size_t msg_len, const char *const *media, size_t media_count) {
    (void)ctx, (void)target, (void)target_len;
    if (media_count > 0) {
        s_media_sends++;
        snprintf(s_last_media, sizeof(s_last_media), "%s", media[0]);
    } else {
        s_sends++;
        snprintf(s_last_text, sizeof(s_last_text), "%.*s", (int)msg_len, msg);
    }
    return HU_OK;
}
static hu_error_t mock_react(void *ctx, const char *target, size_t target_len, int64_t id,
                             hu_reaction_type_t r) {
    (void)ctx, (void)target, (void)target_len, (void)id, (void)r;
    s_reacts++;
    return HU_OK;
}
static hu_error_t mock_chat(void *ctx, hu_allocator_t *alloc, const char *sys, size_t sys_len,
                            const char *msg, size_t msg_len, const char *model, size_t model_len,
                            double temp, char **out, size_t *out_len) {
    (void)ctx, (void)sys, (void)sys_len, (void)msg, (void)msg_len, (void)model, (void)model_len,
        (void)temp;
    s_llm_calls++;
    static const char reply[] = "oh and bring the charger";
    *out = (char *)alloc->alloc(alloc->ctx, sizeof(reply));
    memcpy(*out, reply, sizeof(reply));
    *out_len = sizeof(reply) - 1;
    return HU_OK;
}

static hu_channel_vtable_t s_ch_vt;
static hu_channel_t s_ch;
static hu_provider_vtable_t s_pv_vt;
static hu_provider_t s_pv;
static hu_allocator_t s_alloc;

typedef struct {
    hu_agent_t *agent;
    hu_persona_t *persona;
} fx_t;

/* An agent whose reactive path JUST replied to the contact — exactly the state
 * the daemon is in when it reaches the extras. */
static fx_t fx_make(void) {
    s_sends = s_media_sends = s_reacts = s_llm_calls = 0;
    memset(&s_ch_vt, 0, sizeof(s_ch_vt));
    s_ch_vt.name = mock_name;
    s_ch_vt.send = mock_send;
    s_ch_vt.react = mock_react;
    s_ch.ctx = NULL;
    s_ch.vtable = &s_ch_vt;
    memset(&s_pv_vt, 0, sizeof(s_pv_vt));
    s_pv_vt.chat_with_system = mock_chat;
    s_pv.ctx = NULL;
    s_pv.vtable = &s_pv_vt;
    s_alloc = hu_system_allocator();
    fx_t f;
    f.agent = (hu_agent_t *)calloc(1, sizeof(*f.agent));
    f.persona = (hu_persona_t *)calloc(1, sizeof(*f.persona));
    f.persona->humanization.double_text_probability = 1.0f;
    f.agent->persona = f.persona;
    hu_contact_send_recency_record(&f.agent->contact_send_recency, k_contact, strlen(k_contact),
                                   (int64_t)time(NULL), HU_SEND_PATH_REACTIVE);
    return f;
}

static void fx_free(fx_t *f) {
    hu_spontaneity_set_for_test(-1, NULL, 0u); /* also frees the outcome store */
    free(f->persona);
    free(f->agent);
}

/* "lol" makes the reply self-reaction material and the inbound GIF-worthy;
 * no farewell, midday, empty history: every kind is eligible. */
static hu_spontaneity_turn_t turn_for(fx_t *f) {
    hu_spontaneity_turn_t t;
    memset(&t, 0, sizeof(t));
    t.agent = f->agent;
    t.channel = &s_ch;
    t.alloc = &s_alloc;
    t.contact = k_contact;
    t.contact_len = strlen(k_contact);
    t.send_target = k_contact;
    t.send_target_len = strlen(k_contact);
    t.response = "lol yeah that was wild";
    t.response_len = strlen(t.response);
    t.inbound = "lol that party was wild";
    t.inbound_len = strlen(t.inbound);
    t.hour_local = 12;
    t.gif_available = true;
    t.fallback_sent_id = 77;
    t.classify = &s_pv;
    t.classify_model = "test-model";
    t.classify_model_len = 10;
    t.chosen = HU_SPONT_NONE;
    return t;
}

static hu_spontaneity_rates_t rates_of(double dt, double sr, double gif, double n0) {
    hu_spontaneity_rates_t r;
    r.rate[HU_SPONT_DOUBLE_TEXT] = dt;
    r.rate[HU_SPONT_SELF_REACTION] = sr;
    r.rate[HU_SPONT_GIF] = gif;
    for (int k = 0; k < HU_SPONT_KIND_COUNT; k++)
        r.n0[k] = n0;
    r.double_text_gap_s = -1.0;
    return r;
}

static void run_turn(hu_spontaneity_turn_t *t) {
    hu_daemon_spontaneity_choose(t);
    hu_daemon_spontaneity_double_text(t);
    hu_daemon_spontaneity_self_reaction(t);
}

/* Headline: right after a reactive reply the OFF path never fires (the DEF-15
 * trap: legacy GIF gate closed, legacy double-text deferred), while LIVE
 * reaches the extra and delivers it on the next daemon pass. */
static void spontaneity_live_reachable_after_reactive_reply(void) {
    fx_t f = fx_make();
    hu_spontaneity_rates_t only_dt = rates_of(1.0, 0.0, 0.0, 100.0);
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;

    hu_spontaneity_set_for_test(HU_GATE_OFF, &only_dt, 1u);
    hu_spontaneity_turn_t t = turn_for(&f);
    run_turn(&t);
    HU_ASSERT_FALSE(hu_daemon_spontaneity_gif_open(&t, now_ms));
    HU_ASSERT_EQ(s_llm_calls, 0); /* deferred before generating */
    HU_ASSERT_EQ(s_sends, 0);

    hu_spontaneity_set_for_test(HU_GATE_LIVE, &only_dt, 1u);
    t = turn_for(&f);
    run_turn(&t);
    HU_ASSERT_EQ(t.chosen, HU_SPONT_DOUBLE_TEXT);
    HU_ASSERT_EQ(s_llm_calls, 1);
    HU_ASSERT_EQ(hu_spontaneity_queued_for_test(), 1u);
    HU_ASSERT_EQ(s_sends, 0); /* nothing sent inline */
    hu_daemon_spontaneity_tick(&s_ch, "imessage", (int64_t)now_ms + 1000);
    HU_ASSERT_EQ(s_sends, 1);
    HU_ASSERT_STR_EQ(s_last_text, "oh and bring the charger");
    HU_ASSERT_EQ(hu_spontaneity_queued_for_test(), 0u);
    fx_free(&f);
}

/* CRITICAL 2: the LIVE decide path returns without sleeping (the legacy path
 * slept 10–45 s for the afterthought and 1.5–4.5 s for the self-reaction). */
static void spontaneity_live_decide_path_does_not_sleep(void) {
    fx_t f = fx_make();
    /* GIF rate 0: its fetch belongs to daemon.c's GIF block, not this path */
    hu_spontaneity_rates_t all = rates_of(1.0, 1.0, 0.0, 100.0);
    hu_spontaneity_set_for_test(HU_GATE_LIVE, &all, 5u);
    time_t t0 = time(NULL);
    for (int i = 0; i < 6; i++) {
        hu_spontaneity_turn_t t = turn_for(&f);
        run_turn(&t);
        hu_daemon_spontaneity_tick(&s_ch, "imessage", ((int64_t)time(NULL) + 1) * 1000);
    }
    HU_ASSERT_TRUE(time(NULL) - t0 <= 1);
    HU_ASSERT_EQ(s_sends + s_reacts, 6); /* each turn delivered its one extra */
    fx_free(&f);
}

/* One extra per reply across the three kinds, and WHICH one is sampled, not
 * fixed (double-text first was the old order). */
static void spontaneity_live_one_extra_per_turn_sampled_across_kinds(void) {
    fx_t f = fx_make();
    hu_spontaneity_rates_t all = rates_of(1.0, 1.0, 1.0, 100.0);
    hu_spontaneity_set_for_test(HU_GATE_LIVE, &all, 99u);
    int picks[HU_SPONT_KIND_COUNT] = {0};
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    for (int i = 0; i < 300; i++) {
        hu_spontaneity_turn_t t = turn_for(&f);
        hu_spontaneity_kind_t k = hu_daemon_spontaneity_choose(&t);
        HU_ASSERT_TRUE(k < HU_SPONT_KIND_COUNT);
        picks[k]++;
        /* the GIF block's gate opens only for the chosen kind */
        HU_ASSERT_EQ(hu_daemon_spontaneity_gif_open(&t, now_ms), k == HU_SPONT_GIF);
    }
    for (int k = 0; k < HU_SPONT_KIND_COUNT; k++)
        HU_ASSERT_TRUE(picks[k] > 60); /* ~100 each */

    hu_spontaneity_turn_t t = turn_for(&f);
    run_turn(&t);
    size_t queued = hu_spontaneity_queued_for_test();
    if (t.chosen == HU_SPONT_GIF)
        HU_ASSERT_EQ(queued, 0u); /* the GIF is queued by gif_send, below */
    else
        HU_ASSERT_EQ(queued, 1u);
    fx_free(&f);
}

/* LIVE self-reaction: queued, delivered by the tick as a react, not inline. */
static void spontaneity_live_self_reaction_call_site(void) {
    fx_t f = fx_make();
    hu_spontaneity_rates_t only_sr = rates_of(0.0, 1.0, 0.0, 100.0);
    hu_spontaneity_set_for_test(HU_GATE_LIVE, &only_sr, 3u);
    hu_spontaneity_turn_t t = turn_for(&f);
    run_turn(&t);
    HU_ASSERT_EQ(t.chosen, HU_SPONT_SELF_REACTION);
    HU_ASSERT_EQ(s_reacts, 0);
    hu_daemon_spontaneity_tick(&s_ch, "imessage", ((int64_t)time(NULL) + 1) * 1000);
    HU_ASSERT_EQ(s_reacts, 1);
    HU_ASSERT_EQ(s_sends, 0);
    fx_free(&f);
}

/* LIVE GIF: gif_roll follows the choice, gif_send queues the file, the tick
 * sends it as media and removes it. */
static void spontaneity_live_gif_call_site(void) {
    fx_t f = fx_make();
    hu_spontaneity_rates_t only_gif = rates_of(0.0, 0.0, 1.0, 100.0);
    hu_spontaneity_set_for_test(HU_GATE_LIVE, &only_gif, 4u);
    hu_spontaneity_turn_t t = turn_for(&f);
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    HU_ASSERT_EQ(hu_daemon_spontaneity_choose(&t), HU_SPONT_GIF);
    HU_ASSERT_TRUE(hu_daemon_spontaneity_gif_open(&t, now_ms));
    HU_ASSERT_TRUE(hu_daemon_spontaneity_gif_roll(&t, 1u, 0.0f, now_ms));

    char dir[512], path[600];
    HU_ASSERT_TRUE(hu_test_tmpdir(dir, sizeof(dir), "spont_gif"));
    snprintf(path, sizeof(path), "%s/x.gif", dir);
    FILE *fp = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(fp);
    fputs("GIF89a", fp);
    fclose(fp);
    size_t plen = strlen(path);
    char *owned = (char *)s_alloc.alloc(s_alloc.ctx, plen + 1);
    memcpy(owned, path, plen + 1);
    HU_ASSERT_TRUE(hu_daemon_spontaneity_gif_send(&t, owned, "party", 5, 1u, now_ms));
    HU_ASSERT_EQ(s_media_sends, 0);
    HU_ASSERT_EQ(hu_spontaneity_queued_for_test(), 1u);
    hu_daemon_spontaneity_tick(&s_ch, "imessage", (int64_t)now_ms + 1000);
    HU_ASSERT_EQ(s_media_sends, 1);
    HU_ASSERT_STR_EQ(s_last_media, path);
    HU_ASSERT_TRUE(access(path, F_OK) != 0); /* removed after send */
    rmdir(dir);
    fx_free(&f);
}

/* LIVE without a learned rate never fires, even when every kind is eligible. */
static void spontaneity_live_without_learned_rate_never_fires(void) {
    fx_t f = fx_make();
    hu_spontaneity_rates_t none = rates_of(-1.0, -1.0, -1.0, 100.0);
    hu_spontaneity_set_for_test(HU_GATE_LIVE, &none, 3u);
    for (int i = 0; i < 100; i++) {
        hu_spontaneity_turn_t t = turn_for(&f);
        HU_ASSERT_EQ(hu_daemon_spontaneity_choose(&t), HU_SPONT_NONE);
    }
    fx_free(&f);
}

/* SHADOW changes nothing that is sent: the legacy (deferred) path runs. */
static void spontaneity_shadow_keeps_legacy_result(void) {
    fx_t f = fx_make();
    hu_spontaneity_rates_t all = rates_of(1.0, 1.0, 1.0, 100.0);
    hu_spontaneity_set_for_test(HU_GATE_SHADOW, &all, 7u);
    hu_spontaneity_turn_t t = turn_for(&f);
    run_turn(&t);
    HU_ASSERT_EQ(t.chosen, HU_SPONT_NONE);
    HU_ASSERT_FALSE(hu_daemon_spontaneity_gif_open(&t, (uint64_t)time(NULL) * 1000ULL));
    HU_ASSERT_EQ(hu_spontaneity_queued_for_test(), 0u);
    HU_ASSERT_EQ(s_sends + s_reacts + s_llm_calls, 0);
    fx_free(&f);
}

/* IMPORTANT 3: the firing probability is a learned Beta — prior from Seth's
 * rate and sample size, moved by outcomes — not a static formula. */
static void spontaneity_posterior_learns_from_outcomes(void) {
    fx_t f = fx_make();
    hu_spontaneity_rates_t r = rates_of(0.20, 0.20, 0.20, 20.0);
    hu_spontaneity_set_for_test(HU_GATE_LIVE, &r, 11u);
    double prior = 0.0;
    for (int i = 0; i < 2000; i++)
        prior += hu_spontaneity_sample_p(k_contact, strlen(k_contact), HU_SPONT_DOUBLE_TEXT, &r);
    prior /= 2000.0;
    HU_ASSERT_TRUE(prior > 0.17 && prior < 0.23); /* Beta(4,16): mean 0.20 */

    /* 20 afterthoughts, each delivered and then answered by the contact,
     * through the real queue -> tick -> inbound feedback path. */
    int64_t now = (int64_t)time(NULL) * 1000;
    for (int i = 0; i < 20; i++) {
        hu_spontaneity_turn_t t = turn_for(&f);
        t.chosen = HU_SPONT_DOUBLE_TEXT;
        hu_daemon_spontaneity_double_text(&t);
        hu_daemon_spontaneity_tick(&s_ch, "imessage", now + 1000);
        hu_daemon_spontaneity_on_inbound(k_contact, strlen(k_contact), now + 2000);
    }
    uint64_t succ = 0, fail = 0;
    hu_spontaneity_outcomes_for_test(k_contact, HU_SPONT_DOUBLE_TEXT, &succ, &fail);
    HU_ASSERT_EQ(succ, 20u);
    HU_ASSERT_EQ(fail, 0u);
    double post = 0.0;
    for (int i = 0; i < 2000; i++)
        post += hu_spontaneity_sample_p(k_contact, strlen(k_contact), HU_SPONT_DOUBLE_TEXT, &r);
    post /= 2000.0;
    HU_ASSERT_TRUE(post > 0.55); /* Beta(24,16): mean 0.60 */
    fx_free(&f);
}

/* Silence for the outcome horizon is a failure; a tap on the extra (DEF-8
 * engagement) is a success; a tap on an older message is not. */
static void spontaneity_outcome_silence_fails_tap_succeeds(void) {
    fx_t f = fx_make();
    hu_spontaneity_rates_t r = rates_of(0.5, 0.5, 0.5, 10.0);
    hu_spontaneity_set_for_test(HU_GATE_LIVE, &r, 21u);
    int64_t now = (int64_t)time(NULL) * 1000;
    hu_spontaneity_turn_t t = turn_for(&f);
    t.chosen = HU_SPONT_DOUBLE_TEXT;
    hu_daemon_spontaneity_double_text(&t);
    hu_daemon_spontaneity_tick(&s_ch, "imessage", now);
    hu_daemon_spontaneity_on_engagement(k_contact, strlen(k_contact), now - 60000); /* older */
    hu_daemon_spontaneity_tick(&s_ch, "imessage",
                               now + (int64_t)HU_SPONT_OUTCOME_HORIZON_S * 1000 + 1);
    uint64_t succ = 9, fail = 9;
    hu_spontaneity_outcomes_for_test(k_contact, HU_SPONT_DOUBLE_TEXT, &succ, &fail);
    HU_ASSERT_EQ(succ, 0u);
    HU_ASSERT_EQ(fail, 1u);

    t = turn_for(&f);
    t.chosen = HU_SPONT_DOUBLE_TEXT;
    hu_daemon_spontaneity_double_text(&t);
    hu_daemon_spontaneity_tick(&s_ch, "imessage", now);
    hu_daemon_spontaneity_on_engagement(k_contact, strlen(k_contact), now + 10);
    hu_spontaneity_outcomes_for_test(k_contact, HU_SPONT_DOUBLE_TEXT, &succ, &fail);
    HU_ASSERT_EQ(succ, 1u);
    HU_ASSERT_EQ(fail, 1u);
    fx_free(&f);
}

static void spontaneity_rates_parse_reads_optional_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_spontaneity_rates_t r;
    const char *j = "{\"double_text_rate\": 0.18, \"double_text_n\": 340, \"gif_rate\": 0.02, "
                    "\"n_eff\": 900}";
    HU_ASSERT_EQ(hu_spontaneity_rates_parse(&alloc, j, strlen(j), &r), HU_OK);
    HU_ASSERT_TRUE(r.rate[HU_SPONT_DOUBLE_TEXT] > 0.179 && r.rate[HU_SPONT_DOUBLE_TEXT] < 0.181);
    HU_ASSERT_TRUE(r.n0[HU_SPONT_DOUBLE_TEXT] > 339.0 && r.n0[HU_SPONT_DOUBLE_TEXT] < 341.0);
    HU_ASSERT_TRUE(r.n0[HU_SPONT_GIF] > 899.0);           /* falls back to n_eff */
    HU_ASSERT_TRUE(r.rate[HU_SPONT_SELF_REACTION] < 0.0); /* absent */
    const char *v1 = "{\"schema\": \"learned-style/v1\", \"global\": {\"n\": 900, "
                     "\"self_reaction_rate\": 0.01, \"gif_rate\": null}, \"contacts\": {}}";
    HU_ASSERT_EQ(hu_spontaneity_rates_parse(&alloc, v1, strlen(v1), &r), HU_OK);
    HU_ASSERT_TRUE(r.rate[HU_SPONT_SELF_REACTION] > 0.0099 &&
                   r.rate[HU_SPONT_SELF_REACTION] < 0.0101);
    HU_ASSERT_TRUE(r.rate[HU_SPONT_GIF] < 0.0); /* null = not measured */
    HU_ASSERT_TRUE(r.double_text_gap_s < 0.0);
    const char *gap = "{\"global\": {\"double_text_gap_s\": 95}}";
    HU_ASSERT_EQ(hu_spontaneity_rates_parse(&alloc, gap, strlen(gap), &r), HU_OK);
    HU_ASSERT_TRUE(r.double_text_gap_s > 94.9 && r.double_text_gap_s < 95.1);
    const char *no_n = "{\"double_text_rate\": 0.2}";
    HU_ASSERT_EQ(hu_spontaneity_rates_parse(&alloc, no_n, strlen(no_n), &r), HU_OK);
    HU_ASSERT_TRUE(r.rate[HU_SPONT_DOUBLE_TEXT] < 0.0); /* a rate needs a sample size */
    HU_ASSERT_EQ(hu_spontaneity_rates_parse(&alloc, "[1]", 3, &r), HU_ERR_PARSE);
}

void run_daemon_spontaneity_tests(void) {
    HU_TEST_SUITE("daemon_spontaneity");
    HU_RUN_TEST(spontaneity_live_reachable_after_reactive_reply);
    HU_RUN_TEST(spontaneity_live_decide_path_does_not_sleep);
    HU_RUN_TEST(spontaneity_live_one_extra_per_turn_sampled_across_kinds);
    HU_RUN_TEST(spontaneity_live_self_reaction_call_site);
    HU_RUN_TEST(spontaneity_live_gif_call_site);
    HU_RUN_TEST(spontaneity_live_without_learned_rate_never_fires);
    HU_RUN_TEST(spontaneity_shadow_keeps_legacy_result);
    HU_RUN_TEST(spontaneity_posterior_learns_from_outcomes);
    HU_RUN_TEST(spontaneity_outcome_silence_fails_tap_succeeds);
    HU_RUN_TEST(spontaneity_rates_parse_reads_optional_fields);
}
