/* Learned tapback evidence for director v2: every tapback-vs-text outcome here
 * comes from the fixture profile's numbers, never from the message's words.
 * Hermetic: an in-memory profile, plus one temp-dir file for the loader. */
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/daemon/director.h"
#include "human/daemon/director_tapback.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define S(lit) (lit), (sizeof(lit) - 1)

#define MOM    "+15550000001"
#define FRIEND "+15550000002"
#define CRITIC "+15550000003"

/* Nine contact cells; sorted 0, .01, .05, .2, .25, .3, .45, .5, .6, so the
 * lower quartile (nearest rank 3) is 0.05. */
static const char k_profile[] =
    "{\"schema\":\"learned-style/v1\","
    "\"global\":{\"n\":900,\"len_p50\":30,\"tapback_only_rate\":0.35},"
    "\"contacts\":{"
    "\"" MOM "\":{\"overall\":{\"n\":132,\"tapback_only_rate\":0.30,"
    "\"tapback_disengage_rate\":0.4,\"tapback_disengage_n\":10},"
    "\"buckets\":{"
    "\"shape:question\":{\"n\":40,\"tapback_only_rate\":0.05},"
    "\"shape:story\":{\"n\":12,\"tapback_only_rate\":0.0},"
    "\"shape:casual\":{\"n\":80,\"tapback_only_rate\":0.45,"
    "\"tapback_types\":{\"heart\":0.7,\"haha\":0.3,\"bogus\":0.9}},"
    "\"time:night\":{\"n\":5,\"tapback_only_rate\":0.9}}},"
    "\"" FRIEND "\":{\"overall\":{\"n\":60,\"tapback_only_rate\":0.5},"
    "\"buckets\":{\"shape:casual\":{\"n\":30,\"tapback_only_rate\":0.6},"
    "\"shape:question\":{\"n\":20,\"tapback_only_rate\":0.2}}},"
    "\"" CRITIC "\":{\"overall\":{\"n\":45,\"tapback_only_rate\":0.25},"
    "\"buckets\":{\"shape:question\":{\"n\":30,\"tapback_only_rate\":0.01}}}"
    "}}";

/* A real v1 file today: no reaction fields anywhere. */
static const char k_profile_v1_plain[] =
    "{\"schema\":\"learned-style/v1\",\"global\":{\"n\":900,\"len_p50\":30},"
    "\"contacts\":{\"" MOM "\":{\"overall\":{\"n\":132,\"len_p50\":25}}}}";

static hu_json_value_t *parse(hu_allocator_t *a, const char *json) {
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ((int)hu_json_parse(a, json, strlen(json), &root), (int)HU_OK);
    HU_ASSERT_NOT_NULL(root);
    return root;
}

static hu_director_result_t model_tapback(void) {
    hu_director_result_t r;
    memset(&r, 0, sizeof(r));
    r.action = DIR_TAPBACK;
    r.form = HU_DIR_FORM_TAPBACK;
    r.reaction = HU_REACTION_HEART;
    return r;
}

/* The learned decision for one inbound message from one contact. */
static hu_tapback_src_t decide(const char *json, const char *contact, const char *msg,
                               hu_director_result_t *r) {
    hu_allocator_t a = hu_system_allocator();
    hu_json_value_t *root = parse(&a, json);
    hu_tapback_profile_t tp;
    (void)hu_tapback_profile_from_json(root, contact, strlen(contact),
                                       hu_director_inbound_shape(msg, strlen(msg)), &tp);
    hu_json_free(&a, root);
    *r = model_tapback();
    return hu_director_v2_tapback_check(r, &tp);
}

/* ── shape ──────────────────────────────────────────────────────────── */

static void tapback_shape_follows_the_learner_rule(void) {
    HU_ASSERT_EQ((int)hu_director_inbound_shape(S("ok")), (int)HU_DIR_SHAPE_CASUAL);
    HU_ASSERT_EQ((int)hu_director_inbound_shape(S("Seth your AI is messed up?")),
                 (int)HU_DIR_SHAPE_QUESTION);
    HU_ASSERT_EQ((int)hu_director_inbound_shape(
                     S("Dad and I both tested positive for covid yesterday. Feeling pretty rough "
                       "but ok. How are you doing")),
                 (int)HU_DIR_SHAPE_STORY);
    /* A daemon-injected note is not what they typed: its '?' does not count. */
    HU_ASSERT_EQ((int)hu_director_inbound_shape(S("[They sent a photo: is this a cat?]\nlol")),
                 (int)HU_DIR_SHAPE_CASUAL);
    HU_ASSERT_EQ((int)hu_director_inbound_shape(NULL, 3), (int)HU_DIR_SHAPE_CASUAL);
}

/* ── the profile ────────────────────────────────────────────────────── */

static void tapback_profile_reads_bucket_then_contact_then_global(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_json_value_t *root = parse(&a, k_profile);
    hu_tapback_profile_t tp;
    HU_ASSERT_TRUE(hu_tapback_profile_from_json(root, S(MOM), HU_DIR_SHAPE_QUESTION, &tp));
    HU_ASSERT_STR_EQ(tp.level, "bucket");
    HU_ASSERT_FLOAT_EQ(tp.rate, 0.05f, 1e-6);
    HU_ASSERT_EQ(tp.n, 40u);
    HU_ASSERT_TRUE(hu_tapback_profile_from_json(root, S(CRITIC), HU_DIR_SHAPE_STORY, &tp));
    HU_ASSERT_STR_EQ(tp.level, "contact");
    HU_ASSERT_FLOAT_EQ(tp.rate, 0.25f, 1e-6);
    HU_ASSERT_TRUE(hu_tapback_profile_from_json(root, S("+15559999999"), HU_DIR_SHAPE_CASUAL, &tp));
    HU_ASSERT_STR_EQ(tp.level, "global");
    HU_ASSERT_FLOAT_EQ(tp.rate, 0.35f, 1e-6);
    hu_json_free(&a, root);
}

/* The cutoff is his own lower quartile over contact cells (shape buckets and
 * overall; the time:night bucket is not a shape cell and is left out). */
static void tapback_cutoff_is_the_lower_quartile_of_his_own_rates(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_json_value_t *root = parse(&a, k_profile);
    hu_tapback_profile_t tp;
    (void)hu_tapback_profile_from_json(root, S(MOM), HU_DIR_SHAPE_CASUAL, &tp);
    HU_ASSERT_TRUE(tp.cutoff_found);
    HU_ASSERT_EQ(tp.cells, 9u);
    HU_ASSERT_FLOAT_EQ(tp.cutoff, 0.05f, 1e-6);
    hu_json_free(&a, root);

    /* Fewer than HU_TAPBACK_MIN_CELLS cells: no distribution, no cutoff. */
    static const char few[] =
        "{\"schema\":\"learned-style/v1\",\"global\":{\"tapback_only_rate\":0.3},"
        "\"contacts\":{\"a\":{\"overall\":{\"tapback_only_rate\":0.0},"
        "\"buckets\":{\"shape:question\":{\"tapback_only_rate\":0.0}}}}}";
    root = parse(&a, few);
    (void)hu_tapback_profile_from_json(root, S("a"), HU_DIR_SHAPE_QUESTION, &tp);
    HU_ASSERT_TRUE(tp.found);
    HU_ASSERT_FALSE(tp.cutoff_found);
    hu_json_free(&a, root);
}

static void tapback_profile_without_reaction_fields_is_no_data(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_json_value_t *root = parse(&a, k_profile_v1_plain);
    hu_tapback_profile_t tp;
    HU_ASSERT_FALSE(hu_tapback_profile_from_json(root, S(MOM), HU_DIR_SHAPE_STORY, &tp));
    HU_ASSERT_FALSE(tp.cutoff_found);
    hu_json_free(&a, root);
    root = parse(&a, "{\"schema\":\"learned-style/v2\",\"global\":{\"tapback_only_rate\":0.1}}");
    HU_ASSERT_FALSE(hu_tapback_profile_from_json(root, S(MOM), HU_DIR_SHAPE_STORY, &tp));
    hu_json_free(&a, root);
}

/* ── the learned post-check (the prod cases, decided by data) ───────── */

static void tapback_check_overrides_mom_covid_news_from_her_story_rate(void) {
    hu_director_result_t r;
    hu_tapback_src_t src =
        decide(k_profile, MOM,
               "Dad and I both tested positive for covid yesterday. Feeling pretty rough but ok. "
               "How are you doing",
               &r);
    HU_ASSERT_EQ((int)src, (int)HU_TAPBACK_SRC_LEARNED);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TEXT);
    HU_ASSERT_EQ((int)r.form, (int)HU_DIR_FORM_TEXT);
    HU_ASSERT_EQ((int)r.reaction, (int)HU_REACTION_NONE);
    HU_ASSERT_TRUE(r.direction[0] != '\0');
}

static void tapback_check_overrides_ai_complaint_from_question_rate(void) {
    hu_director_result_t r;
    HU_ASSERT_EQ((int)decide(k_profile, CRITIC, "Seth your AI is messed up?", &r),
                 (int)HU_TAPBACK_SRC_LEARNED);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TEXT);
}

/* Same words, different person: FRIEND does answer questions with only a
 * reaction (20%, above his lower quartile), so the model's choice stands. */
static void tapback_check_keeps_tapback_where_he_does_react(void) {
    hu_director_result_t r;
    HU_ASSERT_EQ((int)decide(k_profile, FRIEND, "Seth your AI is messed up?", &r),
                 (int)HU_TAPBACK_SRC_MODEL);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ((int)decide(k_profile, MOM, "lol", &r), (int)HU_TAPBACK_SRC_MODEL);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ((int)r.reaction, (int)HU_REACTION_HEART);
}

/* No reaction data: the covid news and the complaint both keep the model's
 * tapback. There is no word list behind the missing data. */
static void tapback_check_without_data_keeps_the_model_decision(void) {
    hu_director_result_t r;
    HU_ASSERT_EQ((int)decide(k_profile_v1_plain, MOM,
                             "Dad and I both tested positive for covid yesterday. Feeling pretty "
                             "rough but ok. How are you doing",
                             &r),
                 (int)HU_TAPBACK_SRC_NODATA);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ((int)decide(k_profile_v1_plain, CRITIC, "Seth your AI is messed up?", &r),
                 (int)HU_TAPBACK_SRC_NODATA);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ((int)hu_director_v2_tapback_check(&r, NULL), (int)HU_TAPBACK_SRC_NODATA);
}

static void tapback_check_leaves_text_decisions_alone(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_json_value_t *root = parse(&a, k_profile);
    hu_tapback_profile_t tp;
    (void)hu_tapback_profile_from_json(root, S(MOM), HU_DIR_SHAPE_STORY, &tp);
    hu_json_free(&a, root);
    hu_director_result_t r;
    memset(&r, 0, sizeof(r));
    r.action = DIR_TEXT;
    (void)snprintf(r.direction, sizeof(r.direction), "%s", "keep");
    HU_ASSERT_EQ((int)hu_director_v2_tapback_check(&r, &tp), (int)HU_TAPBACK_SRC_MODEL);
    HU_ASSERT_STR_EQ(r.direction, "keep");
}

/* ── facts and the loader ───────────────────────────────────────────── */

static void tapback_facts_state_his_numbers(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_json_value_t *root = parse(&a, k_profile);
    hu_tapback_profile_t tp;
    (void)hu_tapback_profile_from_json(root, S(MOM), HU_DIR_SHAPE_CASUAL, &tp);
    hu_json_free(&a, root);
    char buf[400];
    size_t n = hu_tapback_profile_facts(&tp, HU_DIR_SHAPE_CASUAL, buf, sizeof(buf));
    HU_ASSERT_EQ(n, strlen(buf));
    HU_ASSERT_STR_CONTAINS(buf, "on casual messages he replies with only a reaction 45% of the "
                                "time (n=80)");
    HU_ASSERT_STR_CONTAINS(buf, "his reactions: heart 70%, haha 30%");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "bogus"); /* only known reaction names reach the prompt */
    HU_ASSERT_STR_CONTAINS(buf, "pushed for a real answer 40% of the time (n=10).");
    HU_ASSERT_STR_NOT_CONTAINS(buf, MOM);

    hu_tapback_profile_t none;
    memset(&none, 0, sizeof(none));
    HU_ASSERT_EQ(hu_tapback_profile_facts(&none, HU_DIR_SHAPE_CASUAL, buf, sizeof(buf)), 0u);
    HU_ASSERT_EQ(hu_tapback_profile_facts(&tp, HU_DIR_SHAPE_CASUAL, buf, 20), 0u);
    HU_ASSERT_STR_EQ(buf, "");
}

static void tapback_profile_load_reads_the_persona_file(void) {
    char dir[] = "/tmp/hu_dir_tapback_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char path[256];
    (void)snprintf(path, sizeof(path), "%s/fx.learned-style.json", dir);
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs(k_profile, f);
    fclose(f);
    setenv("HU_PERSONA_DIR", dir, 1);
    hu_allocator_t a = hu_system_allocator();
    hu_tapback_profile_t tp;
    bool found = hu_tapback_profile_load(&a, S("fx"), S(MOM), HU_DIR_SHAPE_STORY, &tp);
    bool missing = hu_tapback_profile_load(&a, S("nobody"), S(MOM), HU_DIR_SHAPE_STORY, &tp);
    unsetenv("HU_PERSONA_DIR");
    unlink(path);
    rmdir(dir);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_FALSE(missing);
    HU_ASSERT_FALSE(tp.found);
}

void run_director_tapback_tests(void) {
    HU_TEST_SUITE("director_tapback");
    HU_RUN_TEST(tapback_shape_follows_the_learner_rule);
    HU_RUN_TEST(tapback_profile_reads_bucket_then_contact_then_global);
    HU_RUN_TEST(tapback_cutoff_is_the_lower_quartile_of_his_own_rates);
    HU_RUN_TEST(tapback_profile_without_reaction_fields_is_no_data);
    HU_RUN_TEST(tapback_check_overrides_mom_covid_news_from_her_story_rate);
    HU_RUN_TEST(tapback_check_overrides_ai_complaint_from_question_rate);
    HU_RUN_TEST(tapback_check_keeps_tapback_where_he_does_react);
    HU_RUN_TEST(tapback_check_without_data_keeps_the_model_decision);
    HU_RUN_TEST(tapback_check_leaves_text_decisions_alone);
    HU_RUN_TEST(tapback_facts_state_his_numbers);
    HU_RUN_TEST(tapback_profile_load_reads_the_persona_file);
}
