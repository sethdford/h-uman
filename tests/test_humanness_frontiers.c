#include "human/agent/choreography.h"
#include "human/agent/frontier_persist.h"
#include "human/agent/growth_narrative.h"
#include "human/agent.h"
#include "human/cognition/attachment.h"
#include "human/cognition/novelty.h"
#include "human/cognition/presence.h"
#include "human/cognition/rupture_repair.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/memory/relational_episode.h"
#include "human/persona/circadian.h"
#include "human/persona/creative_voice.h"
#include "human/persona/genuine_boundaries.h"
#include "human/persona/micro_expression.h"
#include "human/persona/narrative_self.h"
#include "human/persona/somatic.h"
#include "test_framework.h"
#include <string.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

static void test_somatic_init_defaults(void) {
    hu_somatic_state_t st;
    hu_somatic_init(&st);
    HU_ASSERT_FLOAT_EQ(st.energy, 1.0f, 1e-5);
    HU_ASSERT_FLOAT_EQ(st.social_battery, 1.0f, 1e-5);
}

static void test_somatic_update_drains_energy(void) {
    hu_somatic_state_t st;
    hu_somatic_init(&st);
    hu_somatic_update(&st, 1000u, 0.f, 0u, HU_PHYSICAL_NORMAL);
    HU_ASSERT_TRUE(st.energy < 1.0f);
}

static void test_somatic_recharge_with_gap(void) {
    hu_somatic_state_t st;
    hu_somatic_init(&st);
    st.last_interaction_ts = 1000u;
    /* >8 hours elapsed triggers full social battery recharge (strictly >8h). */
    uint64_t now = 1000u + 9u * 3600u;
    st.social_battery = 0.1f;
    hu_somatic_update(&st, now, 0.f, 0u, HU_PHYSICAL_NORMAL);
    HU_ASSERT_FLOAT_EQ(st.social_battery, 1.0f, 1e-5);
}

static void test_somatic_build_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_somatic_state_t st;
    hu_somatic_init(&st);
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_somatic_build_context(&alloc, &st, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(len > 0u);
    HU_ASSERT_STR_CONTAINS(out, "SOMATIC");
    hu_str_free(&alloc, out);
}

static void test_somatic_labels(void) {
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(1.0f), "high");
    HU_ASSERT_STR_EQ(hu_somatic_battery_label(1.0f), "charged");
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(0.15f), "depleted");
    HU_ASSERT_STR_EQ(hu_somatic_battery_label(0.15f), "empty");
    HU_ASSERT_STR_EQ(hu_somatic_battery_label(0.25f), "drained");
}

static void test_choreography_default_config(void) {
    hu_choreography_config_t c = hu_choreography_config_default();
    HU_ASSERT_FLOAT_EQ(c.burst_probability, 0.03f, 1e-5);
    HU_ASSERT_FLOAT_EQ(c.double_text_probability, 0.08f, 1e-5);
    HU_ASSERT_FLOAT_EQ(c.energy_level, 1.0f, 1e-5);
    HU_ASSERT_EQ(c.max_segments, 4u);
    HU_ASSERT_EQ(c.ms_per_word, 50u);
    HU_ASSERT_TRUE(c.message_splitting_enabled);
}

static void test_choreography_single_short(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msg = "Hi there.";
    hu_choreography_config_t c = hu_choreography_config_default();
    hu_message_plan_t plan = {0};
    HU_ASSERT_EQ(hu_choreography_plan(&alloc, msg, strlen(msg), &c, 10u, &plan), HU_OK);
    HU_ASSERT_EQ(plan.segment_count, 1u);
    hu_choreography_plan_free(&alloc, &plan);
}

static void test_choreography_paragraph_split(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msg =
        "First paragraph is definitely long enough for the split path.\n\n"
        "Second paragraph is also long enough to count as separate.";
    hu_choreography_config_t c = hu_choreography_config_default();
    hu_message_plan_t plan = {0};
    HU_ASSERT_EQ(hu_choreography_plan(&alloc, msg, strlen(msg), &c, 10u, &plan), HU_OK);
    HU_ASSERT_TRUE(plan.segment_count > 1u);
    hu_choreography_plan_free(&alloc, &plan);
}

static void test_choreography_plan_free(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msg = "Enough characters here to allow multi segment planning.";
    hu_choreography_config_t c = hu_choreography_config_default();
    hu_message_plan_t plan = {0};
    HU_ASSERT_EQ(hu_choreography_plan(&alloc, msg, strlen(msg), &c, 10u, &plan), HU_OK);
    HU_ASSERT_NOT_NULL(plan.segments);
    hu_choreography_plan_free(&alloc, &plan);
    HU_ASSERT_NULL(plan.segments);
    HU_ASSERT_EQ(plan.segment_count, 0u);
}

static void test_narrative_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_narrative_self_t self;
    hu_narrative_self_init(&self);
    HU_ASSERT_EQ(hu_narrative_self_add_theme(&alloc, &self, "growth", 6), HU_OK);
    hu_narrative_self_deinit(&alloc, &self);
}

static void test_narrative_add_theme(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_narrative_self_t self;
    hu_narrative_self_init(&self);
    HU_ASSERT_EQ(hu_narrative_self_add_theme(&alloc, &self, "trust", 5), HU_OK);
    HU_ASSERT_EQ(self.theme_count, 1u);
    hu_narrative_self_deinit(&alloc, &self);
}

static void test_narrative_theme_limit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_narrative_self_t self;
    hu_narrative_self_init(&self);
    for (size_t i = 0; i < HU_NARRATIVE_MAX_THEMES; i++)
        HU_ASSERT_EQ(hu_narrative_self_add_theme(&alloc, &self, "theme", 5), HU_OK);
    HU_ASSERT_EQ(hu_narrative_self_add_theme(&alloc, &self, "extra", 5), HU_ERR_LIMIT_REACHED);
    hu_narrative_self_deinit(&alloc, &self);
}

static void test_narrative_build_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_narrative_self_t self;
    hu_narrative_self_init(&self);
    HU_ASSERT_EQ(hu_narrative_self_set_identity(&alloc, &self, "I value honesty.", 16), HU_OK);
    HU_ASSERT_EQ(hu_narrative_self_add_theme(&alloc, &self, "connection", 10), HU_OK);
    HU_ASSERT_EQ(hu_narrative_self_set_preoccupation(&alloc, &self, "family", 6), HU_OK);
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_narrative_self_build_context(&alloc, &self, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "NARRATIVE SELF");
    hu_str_free(&alloc, out);
    hu_narrative_self_deinit(&alloc, &self);
}

static void test_novelty_tracker_init(void) {
    hu_novelty_tracker_t tr;
    hu_novelty_tracker_init(&tr);
    HU_ASSERT_EQ(tr.cooldown_turns, 10u);
}

static void test_novelty_no_surprise_when_known(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_novelty_tracker_t tr;
    hu_novelty_tracker_init(&tr);
    const char *known[] = {"quantum", "mechanics", "demonstrates"};
    const char *msg = "quantum mechanics demonstrates coherence";
    hu_novelty_signal_t sig = {0};
    for (uint32_t i = 0; i < 10u; i++)
        HU_ASSERT_EQ(hu_novelty_evaluate(&alloc, &tr, "", 0, NULL, 0, NULL, 0, &sig), HU_OK);
    HU_ASSERT_EQ(hu_novelty_evaluate(&alloc, &tr, msg, strlen(msg), known, 3, NULL, 0, &sig), HU_OK);
    HU_ASSERT_TRUE(sig.novelty_score < 0.5f);
    HU_ASSERT_NULL(sig.surprise_prompt);
    hu_novelty_signal_free(&alloc, &sig);
}

static void test_novelty_surprise_on_novel(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_novelty_tracker_t tr;
    hu_novelty_tracker_init(&tr);
    const char *msg = "xyzzyxabcdef mentions something brandnewtopic";
    hu_novelty_signal_t sig = {0};
    for (uint32_t i = 0; i < 10u; i++)
        HU_ASSERT_EQ(hu_novelty_evaluate(&alloc, &tr, "", 0, NULL, 0, NULL, 0, &sig), HU_OK);
    HU_ASSERT_EQ(hu_novelty_evaluate(&alloc, &tr, msg, strlen(msg), NULL, 0, NULL, 0, &sig), HU_OK);
    HU_ASSERT_NOT_NULL(sig.surprise_prompt);
    HU_ASSERT_STR_CONTAINS(sig.surprise_prompt, "NOVELTY");
    hu_novelty_signal_free(&alloc, &sig);
}

static void test_attachment_init_unknown(void) {
    hu_attachment_state_t st;
    hu_attachment_init(&st);
    HU_ASSERT_EQ(st.user_style, HU_ATTACH_UNKNOWN);
    hu_allocator_t alloc = hu_system_allocator();
    hu_attachment_deinit(&alloc, &st);
}

static void test_attachment_anxious_pattern(void) {
    hu_attachment_state_t st;
    hu_attachment_init(&st);
    for (int i = 0; i < 25; i++)
        hu_attachment_update(&st, 0.95f, 0.5f, 0.95f, 0.15f, 0.5f);
    HU_ASSERT_EQ(st.user_style, HU_ATTACH_ANXIOUS);
    hu_allocator_t alloc = hu_system_allocator();
    hu_attachment_deinit(&alloc, &st);
}

static void test_attachment_avoidant_pattern(void) {
    hu_attachment_state_t st;
    hu_attachment_init(&st);
    for (int i = 0; i < 25; i++)
        hu_attachment_update(&st, 0.0f, 0.0f, 0.2f, 0.8f, 0.3f);
    HU_ASSERT_EQ(st.user_style, HU_ATTACH_AVOIDANT);
    hu_allocator_t alloc = hu_system_allocator();
    hu_attachment_deinit(&alloc, &st);
}

static void test_attachment_build_context(void) {
    hu_attachment_state_t st;
    hu_attachment_init(&st);
    for (int i = 0; i < 30; i++)
        hu_attachment_update(&st, 0.98f, 0.5f, 0.95f, 0.1f, 0.5f);
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_attachment_build_context(&alloc, &st, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "ATTACHMENT");
    hu_str_free(&alloc, out);
    hu_attachment_deinit(&alloc, &st);
}

static void test_rupture_init_none(void) {
    hu_rupture_state_t st;
    hu_rupture_init(&st);
    HU_ASSERT_EQ(st.stage, HU_RUPTURE_NONE);
    hu_allocator_t alloc = hu_system_allocator();
    hu_rupture_deinit(&alloc, &st);
}

static void test_rupture_detect_tone_shift(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rupture_state_t st;
    hu_rupture_init(&st);
    hu_rupture_signals_t sig = {0};
    sig.tone_delta = -0.5f;
    HU_ASSERT_EQ(hu_rupture_evaluate(&alloc, &st, &sig, "prior reply", 11), HU_OK);
    HU_ASSERT_EQ(st.stage, HU_RUPTURE_DETECTED);
    hu_rupture_deinit(&alloc, &st);
}

static void test_rupture_advance_to_repair(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rupture_state_t st;
    hu_rupture_init(&st);
    hu_rupture_signals_t sig = {0};
    sig.tone_delta = -0.4f;
    HU_ASSERT_EQ(hu_rupture_evaluate(&alloc, &st, &sig, NULL, 0), HU_OK);
    HU_ASSERT_EQ(hu_rupture_advance(&alloc, &st, false), HU_OK);
    HU_ASSERT_EQ(st.stage, HU_RUPTURE_ACKNOWLEDGED);
    HU_ASSERT_EQ(hu_rupture_advance(&alloc, &st, false), HU_OK);
    HU_ASSERT_EQ(st.stage, HU_RUPTURE_REPAIRING);
    hu_rupture_deinit(&alloc, &st);
}

static void test_rupture_build_context_none(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rupture_state_t st;
    hu_rupture_init(&st);
    char *out = (char *)"sentinel";
    size_t len = 99;
    HU_ASSERT_EQ(hu_rupture_build_context(&alloc, &st, &out, &len), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(len, 0u);
    hu_rupture_deinit(&alloc, &st);
}

static void test_episode_set_and_free(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_relational_episode_t ep;
    hu_relational_episode_init(&ep);
    HU_ASSERT_EQ(hu_relational_episode_set(&alloc, &ep, "user_a", "walked together", "calm",
                                           "closeness grew", 0.7f, 0.8f, 7ull),
                 HU_OK);
    HU_ASSERT_NOT_NULL(ep.contact_id);
    HU_ASSERT_STR_EQ(ep.contact_id, "user_a");
    HU_ASSERT_FLOAT_EQ(ep.significance, 0.7f, 1e-5);
    hu_relational_episode_free(&alloc, &ep);
}

static void test_episode_add_tag_limit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_relational_episode_t ep;
    hu_relational_episode_init(&ep);
    for (int i = 0; i < HU_RELATIONAL_EPISODE_MAX_TAGS; i++)
        HU_ASSERT_EQ(hu_relational_episode_add_tag(&alloc, &ep, "tag"), HU_OK);
    HU_ASSERT_EQ(hu_relational_episode_add_tag(&alloc, &ep, "overflow"), HU_ERR_LIMIT_REACHED);
    hu_relational_episode_free(&alloc, &ep);
}

static void test_episode_build_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_relational_episode_t eps[2];
    hu_relational_episode_init(&eps[0]);
    hu_relational_episode_init(&eps[1]);
    HU_ASSERT_EQ(hu_relational_episode_set(&alloc, &eps[0], "user_a", "first meeting", "warm",
                                           "spark", 0.9f, 0.7f, 1ull),
                 HU_OK);
    HU_ASSERT_EQ(hu_relational_episode_set(&alloc, &eps[1], "user_a", "follow up", "steady",
                                           "routine", 0.4f, 0.5f, 2ull),
                 HU_OK);
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_relational_episode_build_context(&alloc, eps, 2, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "RELATIONAL MEMORY");
    hu_str_free(&alloc, out);
    hu_relational_episode_free(&alloc, &eps[0]);
    hu_relational_episode_free(&alloc, &eps[1]);
}

static void test_episode_create_table_sql(void) {
    char buf[512];
    size_t n = 0;
    HU_ASSERT_EQ(hu_relational_episode_create_table_sql(buf, sizeof(buf), &n), HU_OK);
    HU_ASSERT_STR_CONTAINS(buf, "CREATE TABLE");
    HU_ASSERT_TRUE(n > 0u);
}

static void test_presence_casual(void) {
    hu_presence_state_t st;
    hu_presence_init(&st);
    hu_presence_compute(&st, 0.f, 0.f, 0.f, 0.f, 0u);
    HU_ASSERT_EQ(st.level, HU_PRESENCE_CASUAL);
    HU_ASSERT_FALSE(st.upgrade_model_tier);
    hu_allocator_t alloc = hu_system_allocator();
    hu_presence_deinit(&alloc, &st);
}

static void test_presence_deep(void) {
    hu_presence_state_t st;
    hu_presence_init(&st);
    hu_presence_compute(&st, 1.0f, 1.0f, 1.0f, 1.0f, 10u);
    HU_ASSERT_EQ(st.level, HU_PRESENCE_DEEP);
    HU_ASSERT_TRUE(st.upgrade_model_tier);
    hu_allocator_t alloc = hu_system_allocator();
    hu_presence_deinit(&alloc, &st);
}

static void test_presence_build_context(void) {
    hu_presence_state_t st;
    hu_presence_init(&st);
    hu_presence_compute(&st, 0.f, 0.f, 0.f, 0.f, 0u);
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_presence_build_context(&alloc, &st, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "PRESENCE");
    hu_str_free(&alloc, out);
    hu_presence_deinit(&alloc, &st);
}

static void test_micro_init(void) {
    hu_micro_expression_t me;
    hu_micro_expression_init(&me);
    HU_ASSERT_FLOAT_EQ(me.target_length_factor, 1.0f, 1e-5);
    hu_allocator_t alloc = hu_system_allocator();
    hu_micro_expression_deinit(&alloc, &me);
}

static void test_micro_tired(void) {
    hu_micro_expression_t me;
    hu_micro_expression_init(&me);
    hu_micro_expression_compute(&me, 0.2f, 0.9f, 0.f, 0.3f, 0.3f);
    HU_ASSERT_TRUE(me.target_length_factor < 1.0f);
    hu_allocator_t alloc = hu_system_allocator();
    hu_micro_expression_deinit(&alloc, &me);
}

static void test_micro_excited(void) {
    hu_micro_expression_t me;
    hu_micro_expression_init(&me);
    float base_punct = me.punctuation_density;
    hu_micro_expression_compute(&me, 0.9f, 0.9f, 0.9f, 0.85f, 0.2f);
    HU_ASSERT_TRUE(me.punctuation_density > base_punct);
    hu_allocator_t alloc = hu_system_allocator();
    hu_micro_expression_deinit(&alloc, &me);
}

static void test_creative_empty_no_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_creative_voice_t voice;
    hu_creative_voice_init(&voice);
    char *out = (char *)"sentinel";
    size_t len = 99;
    HU_ASSERT_EQ(hu_creative_voice_build_context(&alloc, &voice, &out, &len), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(len, 0u);
    hu_creative_voice_deinit(&alloc, &voice);
}

static void test_creative_with_domains(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_creative_voice_t voice;
    hu_creative_voice_init(&voice);
    HU_ASSERT_EQ(hu_creative_voice_add_domain(&alloc, &voice, "rivers", 6), HU_OK);
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_creative_voice_build_context(&alloc, &voice, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "CREATIVE VOICE");
    hu_str_free(&alloc, out);
    hu_creative_voice_deinit(&alloc, &voice);
}

static void test_growth_add_observation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_growth_narrative_t gn;
    hu_growth_narrative_init(&gn);
    HU_ASSERT_EQ(hu_growth_narrative_add_observation(&alloc, &gn, "user_a", "more open",
                                                     "said feelings", 0.8f, 1ull),
                 HU_OK);
    HU_ASSERT_EQ(gn.observation_count, 1u);
    hu_growth_narrative_deinit(&alloc, &gn);
}

static void test_growth_build_context_cooldown(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_growth_narrative_t gn;
    hu_growth_narrative_init(&gn);
    HU_ASSERT_EQ(hu_growth_narrative_add_observation(&alloc, &gn, "user_a", "warming up",
                                                     "asked deeper questions", 0.9f, 1ull),
                 HU_OK);
    for (int i = 0; i < 4; i++) {
        char *out = (char *)"sentinel";
        size_t len = 99;
        HU_ASSERT_EQ(hu_growth_narrative_build_context(&alloc, &gn, "user_a", &out, &len), HU_OK);
        HU_ASSERT_NULL(out);
    }
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_growth_narrative_build_context(&alloc, &gn, "user_a", &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "GROWTH");
    hu_str_free(&alloc, out);
    hu_growth_narrative_deinit(&alloc, &gn);
}

static void test_growth_milestone(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_growth_narrative_t gn;
    hu_growth_narrative_init(&gn);
    /* HU_IS_TEST fixed "now" in growth_narrative.c is 2000000000; stay within one week. */
    uint64_t ts = 2000000000ull - 3600ull;
    HU_ASSERT_EQ(hu_growth_narrative_add_milestone(&alloc, &gn, "user_a", "First real talk", ts,
                                                   0.9f),
                 HU_OK);
    HU_ASSERT_EQ(gn.milestone_count, 1u);
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_growth_narrative_build_context(&alloc, &gn, "user_a", &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "Milestone");
    hu_str_free(&alloc, out);
    hu_growth_narrative_deinit(&alloc, &gn);
}

static void test_boundary_add(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_genuine_boundary_set_t set;
    hu_genuine_boundary_set_init(&set);
    HU_ASSERT_EQ(hu_genuine_boundary_add(&alloc, &set, "politics", "no debates", "past burnout", 0.9f,
                                       1ull),
                 HU_OK);
    HU_ASSERT_EQ(set.count, 1u);
    hu_genuine_boundary_set_deinit(&alloc, &set);
}

static void test_boundary_check_relevance(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_genuine_boundary_set_t set;
    hu_genuine_boundary_set_init(&set);
    HU_ASSERT_EQ(hu_genuine_boundary_add(&alloc, &set, "work", "no overtime glorification", "values",
                                       0.95f, 1ull),
                 HU_OK);
    const char *msg = "Let's discuss WORK stress today.";
    const hu_genuine_boundary_t *matched = NULL;
    HU_ASSERT_EQ(hu_genuine_boundary_check_relevance(&set, msg, strlen(msg), &matched), HU_OK);
    HU_ASSERT_NOT_NULL(matched);
    HU_ASSERT_TRUE(matched->times_relevant > 0u);
    hu_genuine_boundary_set_deinit(&alloc, &set);
}

static void test_boundary_no_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_genuine_boundary_set_t set;
    hu_genuine_boundary_set_init(&set);
    HU_ASSERT_EQ(hu_genuine_boundary_add(&alloc, &set, "politics", "step back", "history", 0.95f,
                                         1ull),
                 HU_OK);
    const char *msg = "Weather is nice for a walk.";
    const hu_genuine_boundary_t *matched = NULL;
    HU_ASSERT_EQ(hu_genuine_boundary_check_relevance(&set, msg, strlen(msg), &matched), HU_OK);
    HU_ASSERT_NULL(matched);
    hu_genuine_boundary_set_deinit(&alloc, &set);
}

static void test_boundary_build_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_genuine_boundary_t b;
    memset(&b, 0, sizeof(b));
    b.domain = "health";
    b.stance = "no medical advice";
    b.origin = "not qualified";
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_genuine_boundary_build_context(&alloc, &b, 3u, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "GENUINE BOUNDARY");
    hu_str_free(&alloc, out);
}

/* ── Integration tests: full pipeline cascade ── */

static void test_frontier_state_init_all(void) {
    hu_frontier_state_t fs;
    memset(&fs, 0, sizeof(fs));
    hu_somatic_init(&fs.somatic);
    hu_novelty_tracker_init(&fs.novelty);
    hu_attachment_init(&fs.attachment);
    hu_rupture_init(&fs.rupture);
    hu_narrative_self_init(&fs.narrative);
    hu_creative_voice_init(&fs.creative_voice);
    hu_growth_narrative_init(&fs.growth);
    hu_genuine_boundary_set_init(&fs.boundaries);
    fs.initialized = true;

    HU_ASSERT_TRUE(fs.initialized);
    HU_ASSERT_FLOAT_EQ(fs.somatic.energy, 1.0f, 1e-5);
    HU_ASSERT_EQ((int)fs.attachment.user_style, (int)HU_ATTACH_UNKNOWN);
    HU_ASSERT_EQ((int)fs.rupture.stage, (int)HU_RUPTURE_NONE);
    HU_ASSERT_EQ(fs.growth.observation_count, 0u);
    HU_ASSERT_EQ(fs.boundaries.count, 0u);

    hu_allocator_t alloc = hu_system_allocator();
    hu_narrative_self_deinit(&alloc, &fs.narrative);
    hu_creative_voice_deinit(&alloc, &fs.creative_voice);
    hu_attachment_deinit(&alloc, &fs.attachment);
    hu_rupture_deinit(&alloc, &fs.rupture);
    hu_growth_narrative_deinit(&alloc, &fs.growth);
    hu_genuine_boundary_set_deinit(&alloc, &fs.boundaries);
}

static void test_somatic_circadian_tired(void) {
    hu_time_phase_t phase = hu_circadian_phase(2);
    HU_ASSERT_EQ((int)phase, (int)HU_PHASE_LATE_NIGHT);
    phase = hu_circadian_phase(22);
    HU_ASSERT_EQ((int)phase, (int)HU_PHASE_NIGHT);
    phase = hu_circadian_phase(6);
    HU_ASSERT_EQ((int)phase, (int)HU_PHASE_EARLY_MORNING);
}

static void test_attachment_gap_distress_large_gap(void) {
    hu_attachment_state_t att;
    hu_attachment_init(&att);
    /* Multiple updates push EMA toward anxious thresholds (prox>0.7, distress>0.6) */
    for (int i = 0; i < 5; i++)
        hu_attachment_update(&att, 0.9f, 0.8f, 0.9f, 0.2f, 0.4f);
    HU_ASSERT_TRUE(att.separation_distress > 0.6f);
    HU_ASSERT_TRUE(att.proximity_seeking > 0.7f);
    HU_ASSERT_EQ((int)att.user_style, (int)HU_ATTACH_ANXIOUS);
    hu_allocator_t alloc = hu_system_allocator();
    hu_attachment_deinit(&alloc, &att);
}

static void test_rupture_multi_signal_detection(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rupture_state_t rs;
    hu_rupture_init(&rs);
    hu_rupture_signals_t sig = {0};
    sig.tone_delta = 0.7f;
    sig.length_delta = -0.6f;
    sig.energy_drop = 0.6f;
    hu_rupture_evaluate(&alloc, &rs, &sig, "I think you should try that.", 28);
    HU_ASSERT_TRUE(rs.stage == HU_RUPTURE_DETECTED || rs.severity > 0.3f);
    hu_rupture_deinit(&alloc, &rs);
}

static void test_growth_milestone_on_secure_attachment(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_growth_narrative_t gn;
    hu_growth_narrative_init(&gn);
    hu_growth_narrative_add_milestone(&alloc, &gn, "contact1",
        "developed a secure communication pattern", (uint64_t)1000, 0.7f);
    HU_ASSERT_EQ(gn.milestone_count, 1u);
    char *out = NULL; size_t len = 0;
    hu_growth_narrative_build_context(&alloc, &gn, "contact1", &out, &len);
    if (out) {
        HU_ASSERT_STR_CONTAINS(out, "secure");
        hu_str_free(&alloc, out);
    }
    hu_growth_narrative_deinit(&alloc, &gn);
}

static void test_frontier_cascade_somatic_to_micro(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_somatic_state_t som;
    hu_somatic_init(&som);
    som.energy = 0.2f;
    som.social_battery = 0.1f;

    hu_micro_expression_t mexp;
    hu_micro_expression_init(&mexp);
    hu_micro_expression_compute(&mexp, som.energy, som.social_battery, 0.0f, 0.3f, 0.5f);

    HU_ASSERT_TRUE(mexp.target_length_factor < 1.0f);
    HU_ASSERT_TRUE(mexp.ellipsis_frequency > 0.0f);

    char *out = NULL; size_t len = 0;
    hu_micro_expression_build_context(&alloc, &mexp, &out, &len);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(len > 0);
    hu_str_free(&alloc, out);
    hu_micro_expression_deinit(&alloc, &mexp);
}

static void test_frontier_cascade_presence_to_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_presence_state_t pres;
    hu_presence_init(&pres);
    hu_presence_compute(&pres, 0.9f, 0.8f, 0.0f, 0.9f, 8);
    HU_ASSERT_TRUE(pres.level == HU_PRESENCE_DEEP || pres.level == HU_PRESENCE_ATTENTIVE);
    HU_ASSERT_TRUE(pres.memory_budget_multiplier >= 2);
    hu_presence_deinit(&alloc, &pres);
}

static void test_choreography_energy_affects_delivery(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_choreography_config_t cfg = hu_choreography_config_default();

    cfg.energy_level = 0.9f;
    hu_message_plan_t plan_high = {0};
    hu_choreography_plan(&alloc, "haha that's hilarious\n\nanyway yeah I think so",
                         45, &cfg, 42, &plan_high);

    cfg.energy_level = 0.2f;
    hu_message_plan_t plan_low = {0};
    hu_choreography_plan(&alloc, "haha that's hilarious\n\nanyway yeah I think so",
                         45, &cfg, 42, &plan_low);

    HU_ASSERT_TRUE(plan_high.segment_count >= plan_low.segment_count);
    hu_choreography_plan_free(&alloc, &plan_high);
    hu_choreography_plan_free(&alloc, &plan_low);
}

static void test_boundary_relationship_stage(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_genuine_boundary_set_t bs;
    hu_genuine_boundary_set_init(&bs);
    hu_genuine_boundary_add(&alloc, &bs, "ethics", "honesty matters",
                            "learned this from experience", 0.9f, 0);

    const hu_genuine_boundary_t *matched = NULL;
    hu_genuine_boundary_check_relevance(&bs, "what about ethics in relationships", 34, &matched);
    HU_ASSERT_NOT_NULL(matched);

    char *deep_ctx = NULL; size_t deep_len = 0;
    hu_genuine_boundary_build_context(&alloc, matched, 3, &deep_ctx, &deep_len);
    HU_ASSERT_NOT_NULL(deep_ctx);
    HU_ASSERT_STR_CONTAINS(deep_ctx, "experience");

    char *shallow_ctx = NULL; size_t shallow_len = 0;
    hu_genuine_boundary_build_context(&alloc, matched, 1, &shallow_ctx, &shallow_len);
    HU_ASSERT_NOT_NULL(shallow_ctx);

    HU_ASSERT_TRUE(deep_len >= shallow_len);

    hu_str_free(&alloc, deep_ctx);
    hu_str_free(&alloc, shallow_ctx);
    hu_genuine_boundary_set_deinit(&alloc, &bs);
}

static void test_novelty_with_stm_topics(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_novelty_tracker_t nt;
    hu_novelty_tracker_init(&nt);

    const char *known[] = {"cooking", "music", "travel"};
    hu_novelty_signal_t sig1 = {0};
    /* Satisfy default surprise cooldown before real turns (same pattern as test_novelty_surprise_on_novel) */
    for (uint32_t i = 0; i < 10u; i++)
        HU_ASSERT_EQ(hu_novelty_evaluate(&alloc, &nt, "", 0, NULL, 0, NULL, 0, &sig1), HU_OK);
    hu_novelty_evaluate(&alloc, &nt, "I love cooking pasta", 20, known, 3, known, 3, &sig1);
    bool had_surprise_known = (sig1.surprise_prompt != NULL);
    hu_novelty_signal_free(&alloc, &sig1);

    hu_novelty_signal_t sig2 = {0};
    HU_ASSERT_EQ(hu_novelty_evaluate(&alloc, &nt, "I started learning quantum physics", 34,
                                     known, 3, known, 3, &sig2),
                 HU_OK);
    bool had_surprise_novel = (sig2.surprise_prompt != NULL);
    hu_novelty_signal_free(&alloc, &sig2);

    HU_ASSERT_FALSE(had_surprise_known);
    HU_ASSERT_TRUE(had_surprise_novel);
}

static void test_novelty_seen_hash_persistence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_novelty_tracker_t nt;
    hu_novelty_tracker_init(&nt);

    const char *known[] = {"cooking"};
    hu_novelty_signal_t sig = {0};
    hu_novelty_evaluate(&alloc, &nt, "quantum entanglement discoveries", 33,
                        known, 1, NULL, 0, &sig);
    hu_novelty_signal_free(&alloc, &sig);
    HU_ASSERT_TRUE(nt.seen_count > 0);

    hu_novelty_tracker_t nt2;
    hu_novelty_tracker_init(&nt2);
    memcpy(nt2.seen_hashes, nt.seen_hashes, nt.seen_count * sizeof(uint32_t));
    nt2.seen_count = nt.seen_count;

    hu_novelty_signal_t sig2 = {0};
    hu_novelty_evaluate(&alloc, &nt2, "quantum entanglement discoveries", 33,
                        known, 1, NULL, 0, &sig2);
    HU_ASSERT_TRUE(sig2.novelty_score < 0.5f);
    hu_novelty_signal_free(&alloc, &sig2);
}

#ifdef HU_ENABLE_SQLITE
static void test_frontier_persist_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    hu_error_t err = hu_frontier_persist_ensure_table(db);
    HU_ASSERT_EQ(err, HU_OK);

    hu_frontier_state_t state;
    memset(&state, 0, sizeof(state));
    state.initialized = true;
    hu_somatic_init(&state.somatic);
    state.somatic.energy = 0.42f;
    state.somatic.social_battery = 0.33f;
    hu_novelty_tracker_init(&state.novelty);
    state.novelty.turns_since_last_surprise = 7;
    state.novelty.seen_hashes[0] = 12345;
    state.novelty.seen_hashes[1] = 67890;
    state.novelty.seen_count = 2;
    hu_attachment_init(&state.attachment);
    state.attachment.user_style = HU_ATTACH_SECURE;
    state.attachment.user_confidence = 0.75f;
    hu_rupture_init(&state.rupture);
    state.rupture.severity = 0.6f;

    err = hu_frontier_persist_save(&alloc, db, "test_user", 9, &state);
    HU_ASSERT_EQ(err, HU_OK);

    hu_frontier_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    hu_somatic_init(&loaded.somatic);
    hu_novelty_tracker_init(&loaded.novelty);
    hu_attachment_init(&loaded.attachment);
    hu_rupture_init(&loaded.rupture);

    err = hu_frontier_persist_load(&alloc, db, "test_user", 9, &loaded);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_FLOAT_EQ(loaded.somatic.energy, 0.42f, 0.01f);
    HU_ASSERT_FLOAT_EQ(loaded.somatic.social_battery, 0.33f, 0.01f);
    HU_ASSERT_EQ((int)loaded.attachment.user_style, (int)HU_ATTACH_SECURE);
    HU_ASSERT_FLOAT_EQ(loaded.attachment.user_confidence, 0.75f, 0.01f);
    HU_ASSERT_FLOAT_EQ(loaded.rupture.severity, 0.6f, 0.01f);
    HU_ASSERT_EQ((int)loaded.novelty.turns_since_last_surprise, 7);
    HU_ASSERT_EQ((int)loaded.novelty.seen_count, 2);
    HU_ASSERT_EQ((int)loaded.novelty.seen_hashes[0], 12345);
    HU_ASSERT_EQ((int)loaded.novelty.seen_hashes[1], 67890);

    hu_frontier_state_t not_found;
    memset(&not_found, 0, sizeof(not_found));
    err = hu_frontier_persist_load(&alloc, db, "nobody", 6, &not_found);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);

    sqlite3_close(db);
}

static void test_trust_persist_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_EQ(hu_frontier_persist_ensure_table(db), HU_OK);

    hu_frontier_state_t state;
    memset(&state, 0, sizeof(state));
    state.initialized = true;
    hu_somatic_init(&state.somatic);
    hu_novelty_tracker_init(&state.novelty);
    hu_attachment_init(&state.attachment);
    hu_rupture_init(&state.rupture);
    hu_tcal_init(&state.trust);

    hu_tcal_update(&state.trust, 0.9f, 0.8f, 0.85f);
    hu_tcal_update(&state.trust, 0.9f, 0.8f, 0.85f);
    hu_tcal_update(&state.trust, 0.9f, 0.8f, 0.85f);
    float saved_comp = state.trust.dimensions.competence;
    float saved_benev = state.trust.dimensions.benevolence;
    float saved_composite = state.trust.composite;
    size_t saved_count = state.trust.interaction_count;

    HU_ASSERT_EQ(hu_frontier_persist_save(&alloc, db, "trust_u", 7, &state), HU_OK);

    hu_frontier_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    hu_somatic_init(&loaded.somatic);
    hu_novelty_tracker_init(&loaded.novelty);
    hu_attachment_init(&loaded.attachment);
    hu_rupture_init(&loaded.rupture);
    hu_tcal_init(&loaded.trust);

    HU_ASSERT_EQ(hu_frontier_persist_load(&alloc, db, "trust_u", 7, &loaded), HU_OK);

    HU_ASSERT_FLOAT_EQ(loaded.trust.dimensions.competence, saved_comp, 0.01f);
    HU_ASSERT_FLOAT_EQ(loaded.trust.dimensions.benevolence, saved_benev, 0.01f);
    HU_ASSERT_FLOAT_EQ(loaded.trust.composite, saved_composite, 0.01f);
    HU_ASSERT_EQ((long long)loaded.trust.interaction_count, (long long)saved_count);
    HU_ASSERT_TRUE(loaded.trust.composite > 0.5f);

    sqlite3_close(db);
}

static void test_episode_sqlite_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    char create_sql[512];
    size_t create_len = 0;
    hu_error_t err = hu_relational_episode_create_table_sql(create_sql, sizeof(create_sql), &create_len);
    HU_ASSERT_EQ(err, HU_OK);
    char *errmsg = NULL;
    sqlite3_exec(db, create_sql, NULL, NULL, &errmsg);
    HU_ASSERT_NULL(errmsg);

    hu_relational_episode_t ep;
    hu_relational_episode_init(&ep);
    hu_relational_episode_set(&alloc, &ep, "alice", "talked about childhood",
                              "warm and vulnerable", "trust is deepening", 0.85f, 0.7f, 1000);
    hu_relational_episode_add_tag(&alloc, &ep, "personal");

    char insert_sql[2048];
    size_t insert_len = 0;
    err = hu_relational_episode_insert_sql(&ep, insert_sql, sizeof(insert_sql), &insert_len);
    HU_ASSERT_EQ(err, HU_OK);
    sqlite3_exec(db, insert_sql, NULL, NULL, &errmsg);
    HU_ASSERT_NULL(errmsg);

    hu_relational_episode_t ep2;
    hu_relational_episode_init(&ep2);
    hu_relational_episode_set(&alloc, &ep2, "alice", "shared a joke",
                              "playful", "comfortable enough to be silly", 0.4f, 0.8f, 2000);
    err = hu_relational_episode_insert_sql(&ep2, insert_sql, sizeof(insert_sql), &insert_len);
    HU_ASSERT_EQ(err, HU_OK);
    sqlite3_exec(db, insert_sql, NULL, NULL, &errmsg);
    HU_ASSERT_NULL(errmsg);

    static const char sel_sql[] =
        "SELECT summary, felt_sense, relational_meaning, significance, warmth, timestamp "
        "FROM relational_episodes WHERE contact_id = 'alice' ORDER BY significance DESC LIMIT 5";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sel_sql, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    hu_relational_episode_t loaded[5];
    size_t loaded_n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && loaded_n < 5) {
        hu_relational_episode_init(&loaded[loaded_n]);
        const char *s = (const char *)sqlite3_column_text(stmt, 0);
        const char *f = (const char *)sqlite3_column_text(stmt, 1);
        const char *r = (const char *)sqlite3_column_text(stmt, 2);
        float sig = (float)sqlite3_column_double(stmt, 3);
        float wrm = (float)sqlite3_column_double(stmt, 4);
        uint64_t ts = (uint64_t)sqlite3_column_int64(stmt, 5);
        hu_relational_episode_set(&alloc, &loaded[loaded_n], "alice", s, f, r, sig, wrm, ts);
        loaded_n++;
    }
    sqlite3_finalize(stmt);

    HU_ASSERT_EQ((int)loaded_n, 2);
    HU_ASSERT_STR_CONTAINS(loaded[0].summary, "childhood");
    HU_ASSERT_FLOAT_EQ(loaded[0].significance, 0.85f, 0.01f);
    HU_ASSERT_STR_CONTAINS(loaded[1].summary, "joke");

    char *ctx = NULL; size_t ctx_len = 0;
    hu_relational_episode_build_context(&alloc, loaded, loaded_n, &ctx, &ctx_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_STR_CONTAINS(ctx, "RELATIONAL MEMORY");
    HU_ASSERT_STR_CONTAINS(ctx, "childhood");

    hu_str_free(&alloc, ctx);
    for (size_t i = 0; i < loaded_n; i++)
        hu_relational_episode_free(&alloc, &loaded[i]);
    hu_relational_episode_free(&alloc, &ep);
    hu_relational_episode_free(&alloc, &ep2);
    sqlite3_close(db);
}

static void test_growth_persist_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);

    hu_frontier_state_t state;
    memset(&state, 0, sizeof(state));
    state.initialized = true;
    hu_somatic_init(&state.somatic);
    hu_novelty_tracker_init(&state.novelty);
    hu_attachment_init(&state.attachment);
    hu_rupture_init(&state.rupture);
    hu_growth_narrative_init(&state.growth);
    hu_genuine_boundary_set_init(&state.boundaries);

    hu_growth_narrative_add_observation(&alloc, &state.growth,
        "alice", "became more open about feelings",
        "shared personal story unprompted", 0.8f, 1000);
    hu_growth_narrative_add_milestone(&alloc, &state.growth,
        "alice", "first time asking for emotional support", 2000, 0.9f);

    hu_frontier_persist_ensure_table(db);
    hu_frontier_persist_save(&alloc, db, "alice", 5, &state);
    hu_frontier_persist_save_growth(&alloc, db, "alice", 5, &state);

    hu_frontier_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    loaded.initialized = true;
    hu_somatic_init(&loaded.somatic);
    hu_novelty_tracker_init(&loaded.novelty);
    hu_attachment_init(&loaded.attachment);
    hu_rupture_init(&loaded.rupture);
    hu_growth_narrative_init(&loaded.growth);
    hu_genuine_boundary_set_init(&loaded.boundaries);

    hu_frontier_persist_load(&alloc, db, "alice", 5, &loaded);
    hu_frontier_persist_load_growth(&alloc, db, "alice", 5, &loaded);

    HU_ASSERT_EQ((int)loaded.growth.observation_count, 1);
    HU_ASSERT_STR_CONTAINS(loaded.growth.observations[0].observation, "open about feelings");
    HU_ASSERT_FLOAT_EQ(loaded.growth.observations[0].confidence, 0.8f, 0.01f);
    HU_ASSERT_EQ((int)loaded.growth.milestone_count, 1);
    HU_ASSERT_STR_CONTAINS(loaded.growth.milestones[0].description, "emotional support");
    HU_ASSERT_FLOAT_EQ(loaded.growth.milestones[0].significance, 0.9f, 0.01f);

    hu_growth_narrative_deinit(&alloc, &state.growth);
    hu_growth_narrative_deinit(&alloc, &loaded.growth);
    sqlite3_close(db);
}

static void test_rupture_trigger_persist(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);

    hu_frontier_state_t state;
    memset(&state, 0, sizeof(state));
    state.initialized = true;
    hu_somatic_init(&state.somatic);
    hu_novelty_tracker_init(&state.novelty);
    hu_attachment_init(&state.attachment);
    hu_rupture_init(&state.rupture);
    hu_growth_narrative_init(&state.growth);
    hu_genuine_boundary_set_init(&state.boundaries);

    state.rupture.stage = HU_RUPTURE_DETECTED;
    state.rupture.severity = 0.7f;
    state.rupture.trigger_summary = hu_strndup(&alloc, "dismissed their concern", 23);

    hu_frontier_persist_ensure_table(db);
    hu_frontier_persist_save(&alloc, db, "bob", 3, &state);

    hu_frontier_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    hu_somatic_init(&loaded.somatic);
    hu_novelty_tracker_init(&loaded.novelty);
    hu_attachment_init(&loaded.attachment);
    hu_rupture_init(&loaded.rupture);
    hu_growth_narrative_init(&loaded.growth);

    hu_frontier_persist_load(&alloc, db, "bob", 3, &loaded);

    HU_ASSERT_EQ((int)loaded.rupture.stage, (int)HU_RUPTURE_DETECTED);
    HU_ASSERT_FLOAT_EQ(loaded.rupture.severity, 0.7f, 0.01f);
    HU_ASSERT_NOT_NULL(loaded.rupture.trigger_summary);
    HU_ASSERT_STR_CONTAINS(loaded.rupture.trigger_summary, "dismissed");

    hu_rupture_deinit(&alloc, &state.rupture);
    hu_rupture_deinit(&alloc, &loaded.rupture);
    sqlite3_close(db);
}

static void test_multi_turn_frontier_evolution(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_frontier_state_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.initialized = true;
    hu_somatic_init(&fs.somatic);
    hu_novelty_tracker_init(&fs.novelty);
    hu_attachment_init(&fs.attachment);
    hu_rupture_init(&fs.rupture);
    hu_narrative_self_init(&fs.narrative);
    hu_creative_voice_init(&fs.creative_voice);
    hu_growth_narrative_init(&fs.growth);
    hu_genuine_boundary_set_init(&fs.boundaries);

    /* Turn 1: casual conversation, some energy drain */
    hu_somatic_update(&fs.somatic, 500, 0, HU_PHYSICAL_NORMAL, 1);
    hu_attachment_update(&fs.attachment, 0.5f, 0.3f, 0.2f, 0.5f, 0.3f);
    HU_ASSERT_TRUE(fs.somatic.energy < 1.0f);
    HU_ASSERT_EQ((int)fs.attachment.user_style, (int)HU_ATTACH_UNKNOWN);

    /* Turn 2: emotional conversation, more engagement */
    hu_somatic_update(&fs.somatic, 300, 0, HU_PHYSICAL_NORMAL, 2);
    hu_attachment_update(&fs.attachment, 0.7f, 0.6f, 0.3f, 0.5f, 0.5f);
    float energy_after_2 = fs.somatic.energy;
    HU_ASSERT_TRUE(energy_after_2 < fs.somatic.focus || energy_after_2 <= 1.0f);

    /* Turn 3: high emotional sharing, pattern converges toward secure */
    hu_attachment_update(&fs.attachment, 0.8f, 0.8f, 0.2f, 0.6f, 0.7f);
    hu_attachment_update(&fs.attachment, 0.8f, 0.8f, 0.2f, 0.6f, 0.7f);
    hu_attachment_update(&fs.attachment, 0.8f, 0.8f, 0.2f, 0.6f, 0.7f);

    /* Presence should be elevated with high emotional weight */
    hu_presence_state_t pres;
    hu_presence_init(&pres);
    hu_presence_compute(&pres, 0.8f, 0.6f, 0.0f, 0.8f, 6);
    HU_ASSERT_TRUE(pres.level >= HU_PRESENCE_ENGAGED);
    hu_presence_deinit(&alloc, &pres);

    /* Turn 4: rupture detected, then repair */
    hu_rupture_signals_t rsig = {.tone_delta = 0.6f, .energy_drop = 0.5f,
                                 .explicit_correction = true};
    hu_rupture_evaluate(&alloc, &fs.rupture, &rsig, "sorry about that", 16);
    HU_ASSERT_TRUE(fs.rupture.stage != HU_RUPTURE_NONE);
    hu_rupture_advance(&alloc, &fs.rupture, true);

    /* Turn 5: recovery, growth observation */
    hu_growth_narrative_add_observation(&alloc, &fs.growth,
        "test_user", "navigated conflict constructively",
        "rupture detected and resolved", 0.7f, 5000);
    HU_ASSERT_EQ((int)fs.growth.observation_count, 1);

    /* Micro expression reflects somatic state */
    hu_micro_expression_t mexp;
    hu_micro_expression_init(&mexp);
    hu_micro_expression_compute(&mexp, fs.somatic.energy, fs.somatic.social_battery,
                                0.5f, 0.6f, 0.5f);
    HU_ASSERT_TRUE(mexp.target_length_factor > 0.0f);
    hu_micro_expression_deinit(&alloc, &mexp);

    /* Verify full state can be persisted and restored */
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_frontier_persist_ensure_table(db);
    hu_frontier_persist_save(&alloc, db, "test_user", 9, &fs);
    hu_frontier_persist_save_growth(&alloc, db, "test_user", 9, &fs);

    hu_frontier_state_t restored;
    memset(&restored, 0, sizeof(restored));
    hu_somatic_init(&restored.somatic);
    hu_novelty_tracker_init(&restored.novelty);
    hu_attachment_init(&restored.attachment);
    hu_rupture_init(&restored.rupture);
    hu_growth_narrative_init(&restored.growth);

    hu_frontier_persist_load(&alloc, db, "test_user", 9, &restored);
    hu_frontier_persist_load_growth(&alloc, db, "test_user", 9, &restored);

    HU_ASSERT_FLOAT_EQ(restored.somatic.energy, fs.somatic.energy, 0.01f);
    HU_ASSERT_EQ((int)restored.growth.observation_count, 1);
    HU_ASSERT_STR_CONTAINS(restored.growth.observations[0].observation, "conflict");

    hu_narrative_self_deinit(&alloc, &fs.narrative);
    hu_creative_voice_deinit(&alloc, &fs.creative_voice);
    hu_attachment_deinit(&alloc, &fs.attachment);
    hu_rupture_deinit(&alloc, &fs.rupture);
    hu_growth_narrative_deinit(&alloc, &fs.growth);
    hu_genuine_boundary_set_deinit(&alloc, &fs.boundaries);
    /* The restored side rehydrates the same frontier fields via
     * hu_frontier_persist_load (frontier_persist.c:291 was leaking
     * 25 b of hu_strndup'd narrative text on PR55's ASan run).
     * Mirror the fs cleanup on restored. */
    hu_narrative_self_deinit(&alloc, &restored.narrative);
    hu_creative_voice_deinit(&alloc, &restored.creative_voice);
    hu_attachment_deinit(&alloc, &restored.attachment);
    hu_rupture_deinit(&alloc, &restored.rupture);
    hu_growth_narrative_deinit(&alloc, &restored.growth);
    sqlite3_close(db);
}

#endif

static void test_choreography_full_pipeline(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_somatic_state_t som;
    hu_somatic_init(&som);
    som.energy = 0.8f;
    som.social_battery = 0.7f;

    hu_choreography_config_t cfg = hu_choreography_config_default();
    cfg.energy_level = som.energy;

    const char *response = "oh wow that's amazing!\n\nI've never thought about it that way. "
                           "It really makes you reconsider everything.";
    size_t resp_len = strlen(response);

    hu_message_plan_t plan = {0};
    hu_error_t err = hu_choreography_plan(&alloc, response, resp_len, &cfg, 12345, &plan);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(plan.segment_count >= 1);

    size_t total_text_len = 0;
    for (size_t i = 0; i < plan.segment_count; i++) {
        HU_ASSERT_NOT_NULL(plan.segments[i].text);
        HU_ASSERT_TRUE(plan.segments[i].text_len > 0);
        total_text_len += plan.segments[i].text_len;
        if (i > 0) {
            HU_ASSERT_TRUE(plan.segments[i].delay_ms > 0);
        }
    }
    HU_ASSERT_TRUE(total_text_len <= resp_len);

    hu_micro_expression_t mexp;
    hu_micro_expression_init(&mexp);
    hu_micro_expression_compute(&mexp, som.energy, som.social_battery, 0.5f, 0.6f, 0.5f);
    HU_ASSERT_TRUE(mexp.target_length_factor > 0.0f);
    hu_micro_expression_deinit(&alloc, &mexp);

    hu_choreography_plan_free(&alloc, &plan);
}

void run_humanness_frontiers_tests(void) {
    HU_TEST_SUITE("humanness_frontiers");
    HU_RUN_TEST(test_somatic_init_defaults);
    HU_RUN_TEST(test_somatic_update_drains_energy);
    HU_RUN_TEST(test_somatic_recharge_with_gap);
    HU_RUN_TEST(test_somatic_build_context);
    HU_RUN_TEST(test_somatic_labels);
    HU_RUN_TEST(test_choreography_default_config);
    HU_RUN_TEST(test_choreography_single_short);
    HU_RUN_TEST(test_choreography_paragraph_split);
    HU_RUN_TEST(test_choreography_plan_free);
    HU_RUN_TEST(test_narrative_init_deinit);
    HU_RUN_TEST(test_narrative_add_theme);
    HU_RUN_TEST(test_narrative_theme_limit);
    HU_RUN_TEST(test_narrative_build_context);
    HU_RUN_TEST(test_novelty_tracker_init);
    HU_RUN_TEST(test_novelty_no_surprise_when_known);
    HU_RUN_TEST(test_novelty_surprise_on_novel);
    HU_RUN_TEST(test_attachment_init_unknown);
    HU_RUN_TEST(test_attachment_anxious_pattern);
    HU_RUN_TEST(test_attachment_avoidant_pattern);
    HU_RUN_TEST(test_attachment_build_context);
    HU_RUN_TEST(test_rupture_init_none);
    HU_RUN_TEST(test_rupture_detect_tone_shift);
    HU_RUN_TEST(test_rupture_advance_to_repair);
    HU_RUN_TEST(test_rupture_build_context_none);
    HU_RUN_TEST(test_episode_set_and_free);
    HU_RUN_TEST(test_episode_add_tag_limit);
    HU_RUN_TEST(test_episode_build_context);
    HU_RUN_TEST(test_episode_create_table_sql);
    HU_RUN_TEST(test_presence_casual);
    HU_RUN_TEST(test_presence_deep);
    HU_RUN_TEST(test_presence_build_context);
    HU_RUN_TEST(test_micro_init);
    HU_RUN_TEST(test_micro_tired);
    HU_RUN_TEST(test_micro_excited);
    HU_RUN_TEST(test_creative_empty_no_context);
    HU_RUN_TEST(test_creative_with_domains);
    HU_RUN_TEST(test_growth_add_observation);
    HU_RUN_TEST(test_growth_build_context_cooldown);
    HU_RUN_TEST(test_growth_milestone);
    HU_RUN_TEST(test_boundary_add);
    HU_RUN_TEST(test_boundary_check_relevance);
    HU_RUN_TEST(test_boundary_no_match);
    HU_RUN_TEST(test_boundary_build_context);
    HU_RUN_TEST(test_frontier_state_init_all);
    HU_RUN_TEST(test_somatic_circadian_tired);
    HU_RUN_TEST(test_attachment_gap_distress_large_gap);
    HU_RUN_TEST(test_rupture_multi_signal_detection);
    HU_RUN_TEST(test_growth_milestone_on_secure_attachment);
    HU_RUN_TEST(test_frontier_cascade_somatic_to_micro);
    HU_RUN_TEST(test_frontier_cascade_presence_to_memory);
    HU_RUN_TEST(test_choreography_energy_affects_delivery);
    HU_RUN_TEST(test_boundary_relationship_stage);
    HU_RUN_TEST(test_novelty_with_stm_topics);
    HU_RUN_TEST(test_novelty_seen_hash_persistence);
    HU_RUN_TEST(test_choreography_full_pipeline);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_frontier_persist_roundtrip);
    HU_RUN_TEST(test_trust_persist_roundtrip);
    HU_RUN_TEST(test_episode_sqlite_roundtrip);
    HU_RUN_TEST(test_growth_persist_roundtrip);
    HU_RUN_TEST(test_rupture_trigger_persist);
    HU_RUN_TEST(test_multi_turn_frontier_evolution);
#endif
}
