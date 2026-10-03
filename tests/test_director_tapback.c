/* Learned behaviour for director v2: every tapback-vs-text outcome here comes
 * from the fixture profile's numbers, never from the message's words.
 * Hermetic: in-memory profiles, plus one temp-dir file for the loader. */
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
#define QUIET  "+15550000004"
#define LURKER "+15550000005"

/* Bucket cells counted (n >= 3): (0.0,12) (0.01,30) (0.05,40) (0.2,20)
 * (0.45,80) (0.6,30) -> total 212, a quarter is 53, the cumulative weight
 * first reaches it at 0.05. FRIEND's story bucket (n=2) and MOM's time:night
 * bucket (not a shape) are left out. Unweighted, the quartile would be 0.01.
 *
 * Overall cells counted (n >= 5): (0.1,20) (0.25,45) (0.30,132) (0.5,60)
 * -> total 257, a quarter is 64.25, reached at 0.25. LURKER (n=4) is out. */
static const char k_profile[] =
    "{\"schema\":\"learned-style/v1\","
    "\"global\":{\"n\":900,\"latency_p50_s\":300,\"tapback_only_rate\":0.35},"
    "\"contacts\":{"
    "\"" MOM "\":{\"overall\":{\"n\":132,\"latency_p50_s\":240,\"tapback_only_rate\":0.30,"
    "\"voice_memo_rate\":0.12,\"gif_rate\":0.0,"
    "\"tapback_disengage_rate\":0.4,\"tapback_disengage_n\":10},"
    "\"buckets\":{"
    "\"shape:question\":{\"n\":40,\"latency_p50_s\":45,\"tapback_only_rate\":0.05},"
    "\"shape:story\":{\"n\":12,\"latency_p50_s\":null,\"tapback_only_rate\":0.0},"
    "\"shape:casual\":{\"n\":80,\"tapback_only_rate\":0.45,"
    "\"tapback_types\":{\"heart\":0.7,\"haha\":0.3,\"bogus\":0.9}},"
    "\"time:night\":{\"n\":5,\"tapback_only_rate\":0.9}}},"
    "\"" FRIEND "\":{\"overall\":{\"n\":60,\"tapback_only_rate\":0.5},"
    "\"buckets\":{\"shape:casual\":{\"n\":30,\"tapback_only_rate\":0.6},"
    "\"shape:question\":{\"n\":20,\"tapback_only_rate\":0.2},"
    "\"shape:story\":{\"n\":2,\"tapback_only_rate\":0.0}}},"
    "\"" CRITIC "\":{\"overall\":{\"n\":45,\"tapback_only_rate\":0.25},"
    "\"buckets\":{\"shape:question\":{\"n\":30,\"tapback_only_rate\":0.01}}},"
    "\"" QUIET "\":{\"overall\":{\"n\":20,\"tapback_only_rate\":0.1}},"
    "\"" LURKER "\":{\"overall\":{\"n\":4,\"tapback_only_rate\":0.9}}"
    "}}";

/* A real v1 file today: no reaction fields anywhere. */
static const char k_profile_v1_plain[] =
    "{\"schema\":\"learned-style/v1\",\"global\":{\"n\":900,\"len_p50\":30},"
    "\"contacts\":{\"" MOM "\":{\"overall\":{\"n\":132,\"len_p50\":25}}}}";

static const char k_covid[] = "Dad and I both tested positive for covid yesterday. Feeling "
                              "pretty rough but ok. How are you doing";

static hu_json_value_t *parse(hu_allocator_t *a, const char *json) {
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ((int)hu_json_parse(a, json, strlen(json), &root), (int)HU_OK);
    HU_ASSERT_NOT_NULL(root);
    return root;
}

static hu_tapback_profile_t profile_for(const char *json, const char *contact,
                                        hu_dir_shape_t shape) {
    hu_allocator_t a = hu_system_allocator();
    hu_json_value_t *root = parse(&a, json);
    hu_tapback_profile_t tp;
    (void)hu_tapback_profile_from_json(root, contact, strlen(contact), shape, &tp);
    hu_json_free(&a, root);
    return tp;
}

static hu_director_result_t model_tapback(void) {
    hu_director_result_t r;
    memset(&r, 0, sizeof(r));
    r.action = DIR_TAPBACK;
    r.form = HU_DIR_FORM_TAPBACK;
    r.reaction = HU_REACTION_HEART;
    r.delay_s = 17;
    (void)snprintf(r.direction, sizeof(r.direction), "%s", "she's worried ; just react ; nothing");
    return r;
}

/* The learned decision for one inbound message from one contact. */
static hu_tapback_src_t decide(const char *json, const char *contact, const char *msg,
                               hu_director_result_t *r) {
    hu_tapback_profile_t tp =
        profile_for(json, contact, hu_director_inbound_shape(msg, strlen(msg)));
    *r = model_tapback();
    return hu_director_v2_tapback_check(r, &tp);
}

/* ── shape: #586's own vectors, so the two cannot drift ─────────────── */

static void tapback_shape_matches_586_vectors(void) {
    static const struct {
        const char *in;
        hu_dir_shape_t want;
    } v[] = {
        {"you coming tonight?", HU_DIR_SHAPE_QUESTION},
        {"ok", HU_DIR_SHAPE_CASUAL},
        {"", HU_DIR_SHAPE_CASUAL},
        {"lol yes", HU_DIR_SHAPE_CASUAL},
        {"So today was wild. Work ran late and then the car wouldn't start. Ended up getting a "
         "ride home from Dave.",
         HU_DIR_SHAPE_STORY},
        {"Went to the store. Got milk.", HU_DIR_SHAPE_CASUAL},
        {"I don't even know where to start with this week honestly, it has been one thing after "
         "another and I'm tired",
         HU_DIR_SHAPE_CASUAL},
        {"We closed on the house!!! Keys tomorrow. Movers Friday. I can't believe it's actually "
         "happening, finally.",
         HU_DIR_SHAPE_STORY},
        {"what?? no way", HU_DIR_SHAPE_QUESTION},
        {"Long day... really long. Talk later.", HU_DIR_SHAPE_CASUAL},
        {"So today was wild.\nWork ran late and the car wouldn't start.\nEnded up getting a ride "
         "home from Dave.",
         HU_DIR_SHAPE_STORY},
        {"  ok \n", HU_DIR_SHAPE_CASUAL},
        /* injected notes, #586 shape_inbound_ignores_injected_notes */
        {"lol look\n[They sent a photo: a golden retriever wearing sunglasses on a beach at "
         "sunset. The dog is sitting on a towel. There is a cooler. Waves behind.]",
         HU_DIR_SHAPE_CASUAL},
        {"ok\n[Audio transcription: are you coming?]\n[They sent a video]", HU_DIR_SHAPE_CASUAL},
        {"[They sent a picture that didn't load on your phone \xE2\x80\x94 you can't see it]",
         HU_DIR_SHAPE_CASUAL},
        {"[sigh] what now?", HU_DIR_SHAPE_QUESTION}, /* typed brackets are kept */
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++)
        HU_ASSERT_EQ((int)hu_director_inbound_shape(v[i].in, strlen(v[i].in)), (int)v[i].want);
    char s[160];
    memset(s, 'a', sizeof(s));
    HU_ASSERT_EQ((int)hu_director_inbound_shape(s, 140), (int)HU_DIR_SHAPE_STORY);
    HU_ASSERT_EQ((int)hu_director_inbound_shape(s, 139), (int)HU_DIR_SHAPE_CASUAL);
    HU_ASSERT_EQ((int)hu_director_inbound_shape(NULL, 3), (int)HU_DIR_SHAPE_CASUAL);
    /* Only known daemon notes are dropped: an unknown bracketed line stays. */
    HU_ASSERT_EQ((int)hu_director_inbound_shape(S("[Some other note: really?]")),
                 (int)HU_DIR_SHAPE_QUESTION);
}

/* ── the profile ────────────────────────────────────────────────────── */

static void tapback_profile_reads_bucket_then_contact_then_global(void) {
    hu_tapback_profile_t tp = profile_for(k_profile, MOM, HU_DIR_SHAPE_QUESTION);
    HU_ASSERT_STR_EQ(tp.level, "bucket");
    HU_ASSERT_FLOAT_EQ(tp.rate, 0.05f, 1e-6);
    HU_ASSERT_EQ(tp.n, 40u);
    tp = profile_for(k_profile, CRITIC, HU_DIR_SHAPE_STORY);
    HU_ASSERT_STR_EQ(tp.level, "contact");
    HU_ASSERT_FLOAT_EQ(tp.rate, 0.25f, 1e-6);
    tp = profile_for(k_profile, "+15559999999", HU_DIR_SHAPE_CASUAL);
    HU_ASSERT_STR_EQ(tp.level, "global");
    HU_ASSERT_FLOAT_EQ(tp.rate, 0.35f, 1e-6);
    /* A bucket under the learner's n >= 3 is not used: FRIEND's story falls
     * through to FRIEND overall. */
    tp = profile_for(k_profile, FRIEND, HU_DIR_SHAPE_STORY);
    HU_ASSERT_STR_EQ(tp.level, "contact");
    HU_ASSERT_FLOAT_EQ(tp.rate, 0.5f, 1e-6);
}

static void tapback_bucket_cutoff_is_n_weighted_over_bucket_cells(void) {
    hu_tapback_profile_t tp = profile_for(k_profile, MOM, HU_DIR_SHAPE_CASUAL);
    HU_ASSERT_TRUE(tp.cutoff_found);
    HU_ASSERT_EQ(tp.cells, 6u);                 /* no overall cells, no n < 3, no time:* bucket */
    HU_ASSERT_FLOAT_EQ(tp.cutoff, 0.05f, 1e-6); /* unweighted would be 0.01 */
}

static void tapback_contact_cutoff_comes_from_overall_cells(void) {
    hu_tapback_profile_t tp = profile_for(k_profile, CRITIC, HU_DIR_SHAPE_STORY);
    HU_ASSERT_STR_EQ(tp.level, "contact");
    HU_ASSERT_TRUE(tp.cutoff_found);
    HU_ASSERT_EQ(tp.cells, 4u); /* LURKER n=4 < 5 is left out */
    HU_ASSERT_FLOAT_EQ(tp.cutoff, 0.25f, 1e-6);
}

static void tapback_global_lookup_has_no_cutoff(void) {
    hu_tapback_profile_t tp = profile_for(k_profile, "+15559999999", HU_DIR_SHAPE_CASUAL);
    HU_ASSERT_TRUE(tp.found);
    HU_ASSERT_FALSE(tp.cutoff_found);
    hu_director_result_t r = model_tapback();
    HU_ASSERT_EQ((int)hu_director_v2_tapback_check(&r, &tp), (int)HU_TAPBACK_SRC_NODATA);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TAPBACK);
}

/* Every peer cell 0: he never answers with only a tapback, so the cutoff is
 * 0 and a tapback-only choice is overridden. That is the data, by design. */
static void tapback_all_zero_profile_overrides_every_tapback(void) {
    static const char zeros[] =
        "{\"schema\":\"learned-style/v1\",\"global\":{\"n\":100,\"tapback_only_rate\":0},"
        "\"contacts\":{"
        "\"a\":{\"buckets\":{\"shape:casual\":{\"n\":9,\"tapback_only_rate\":0},"
        "\"shape:question\":{\"n\":9,\"tapback_only_rate\":0}}},"
        "\"b\":{\"buckets\":{\"shape:casual\":{\"n\":9,\"tapback_only_rate\":0},"
        "\"shape:story\":{\"n\":9,\"tapback_only_rate\":0}}}}}";
    hu_director_result_t r;
    HU_ASSERT_EQ((int)decide(zeros, "a", "lol", &r), (int)HU_TAPBACK_SRC_LEARNED);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TEXT);
}

static void tapback_too_few_cells_is_no_data(void) {
    static const char few[] =
        "{\"schema\":\"learned-style/v1\",\"global\":{\"tapback_only_rate\":0.3},"
        "\"contacts\":{\"a\":{\"buckets\":{\"shape:question\":{\"n\":9,\"tapback_only_rate\":0}}}"
        "}}";
    hu_tapback_profile_t tp = profile_for(few, "a", HU_DIR_SHAPE_QUESTION);
    HU_ASSERT_TRUE(tp.found);
    HU_ASSERT_FALSE(tp.cutoff_found);
}

static void tapback_profile_without_reaction_fields_is_no_data(void) {
    hu_tapback_profile_t tp = profile_for(k_profile_v1_plain, MOM, HU_DIR_SHAPE_STORY);
    HU_ASSERT_FALSE(tp.found);
    HU_ASSERT_FALSE(tp.cutoff_found);
    tp = profile_for("{\"schema\":\"learned-style/v2\",\"global\":{\"tapback_only_rate\":0.1}}",
                     MOM, HU_DIR_SHAPE_STORY);
    HU_ASSERT_FALSE(tp.found);
}

/* ── the learned post-check (the prod cases, decided by data) ───────── */

static void tapback_check_overrides_mom_covid_news_from_her_story_rate(void) {
    hu_director_result_t r;
    HU_ASSERT_EQ((int)decide(k_profile, MOM, k_covid, &r), (int)HU_TAPBACK_SRC_LEARNED);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TEXT);
    HU_ASSERT_EQ((int)r.form, (int)HU_DIR_FORM_TEXT);
    HU_ASSERT_EQ((int)r.reaction, (int)HU_REACTION_NONE);
    /* No canned values: the model's own delay and direction stand. */
    HU_ASSERT_EQ(r.delay_s, 17u);
    HU_ASSERT_STR_EQ(r.direction, "she's worried ; just react ; nothing");
}

static void tapback_check_overrides_ai_complaint_from_question_rate(void) {
    hu_director_result_t r;
    HU_ASSERT_EQ((int)decide(k_profile, CRITIC, "Seth your AI is messed up?", &r),
                 (int)HU_TAPBACK_SRC_LEARNED);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TEXT);
}

/* Same words, different person: FRIEND does answer questions with only a
 * reaction (20%, above the cutoff), so the model's choice stands. */
static void tapback_check_keeps_tapback_where_he_does_react(void) {
    hu_director_result_t r;
    HU_ASSERT_EQ((int)decide(k_profile, FRIEND, "Seth your AI is messed up?", &r),
                 (int)HU_TAPBACK_SRC_MODEL);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ((int)decide(k_profile, MOM, "lol", &r), (int)HU_TAPBACK_SRC_MODEL);
    HU_ASSERT_EQ((int)r.reaction, (int)HU_REACTION_HEART);
}

/* No reaction data: the covid news and the complaint both keep the model's
 * tapback. There is no word list behind the missing data. */
static void tapback_check_without_data_keeps_the_model_decision(void) {
    hu_director_result_t r;
    HU_ASSERT_EQ((int)decide(k_profile_v1_plain, MOM, k_covid, &r), (int)HU_TAPBACK_SRC_NODATA);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ((int)decide(k_profile_v1_plain, CRITIC, "Seth your AI is messed up?", &r),
                 (int)HU_TAPBACK_SRC_NODATA);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ((int)hu_director_v2_tapback_check(&r, NULL), (int)HU_TAPBACK_SRC_NODATA);
}

static void tapback_check_leaves_text_decisions_alone(void) {
    hu_tapback_profile_t tp = profile_for(k_profile, MOM, HU_DIR_SHAPE_STORY);
    hu_director_result_t r;
    memset(&r, 0, sizeof(r));
    r.action = DIR_TEXT;
    (void)snprintf(r.direction, sizeof(r.direction), "%s", "keep");
    HU_ASSERT_EQ((int)hu_director_v2_tapback_check(&r, &tp), (int)HU_TAPBACK_SRC_MODEL);
    HU_ASSERT_STR_EQ(r.direction, "keep");
}

/* ── facts and the loader ───────────────────────────────────────────── */

static void tapback_facts_state_his_numbers(void) {
    hu_tapback_profile_t tp = profile_for(k_profile, MOM, HU_DIR_SHAPE_CASUAL);
    char buf[400];
    size_t n = hu_tapback_profile_facts(&tp, HU_DIR_SHAPE_CASUAL, buf, sizeof(buf));
    HU_ASSERT_EQ(n, strlen(buf));
    HU_ASSERT_STR_CONTAINS(buf, "he usually answers them after about 4 minutes;");
    HU_ASSERT_STR_CONTAINS(buf, "on casual messages he replies with only a reaction 45% of the "
                                "time (n=80)");
    HU_ASSERT_STR_CONTAINS(buf, "his reactions: heart 70%, haha 30%");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "bogus"); /* only known reaction names reach the prompt */
    HU_ASSERT_STR_CONTAINS(buf, "pushed for a real answer 40% of the time (n=10);");
    HU_ASSERT_STR_CONTAINS(buf, "of his replies to them: a voice memo 12%, a GIF 0%.");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "shared song"); /* absent field: omitted */
    HU_ASSERT_STR_NOT_CONTAINS(buf, MOM);
    /* The question bucket has its own delay. */
    tp = profile_for(k_profile, MOM, HU_DIR_SHAPE_QUESTION);
    (void)hu_tapback_profile_facts(&tp, HU_DIR_SHAPE_QUESTION, buf, sizeof(buf));
    HU_ASSERT_STR_CONTAINS(buf, "after about 45 seconds;");

    hu_tapback_profile_t none;
    memset(&none, 0, sizeof(none));
    HU_ASSERT_EQ(hu_tapback_profile_facts(&none, HU_DIR_SHAPE_CASUAL, buf, sizeof(buf)), 0u);
    HU_ASSERT_EQ(hu_tapback_profile_facts(&tp, HU_DIR_SHAPE_CASUAL, buf, 20), 0u);
    HU_ASSERT_STR_EQ(buf, "");
}

static bool write_file(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fputs(body, f);
    fclose(f);
    return true;
}

static void tapback_profile_load_caches_by_file_version(void) {
    char dir[] = "/tmp/hu_dir_tapback_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char path[256];
    (void)snprintf(path, sizeof(path), "%s/fx.learned-style.json", dir);
    setenv("HU_PERSONA_DIR", dir, 1);
    hu_tapback_profile_cache_reset();
    hu_tapback_profile_t a, b, c;
    bool w1 = write_file(path, k_profile);
    bool f1 = hu_tapback_profile_load(S("fx"), S(MOM), HU_DIR_SHAPE_STORY, &a);
    bool w2 = write_file(path, k_profile_v1_plain); /* a new version: different size */
    bool f2 = hu_tapback_profile_load(S("fx"), S(MOM), HU_DIR_SHAPE_STORY, &b);
    unlink(path);
    bool f3 = hu_tapback_profile_load(S("fx"), S(MOM), HU_DIR_SHAPE_STORY, &c);
    hu_tapback_profile_cache_reset();
    unsetenv("HU_PERSONA_DIR");
    rmdir(dir);
    HU_ASSERT_TRUE(w1 && w2);
    HU_ASSERT_TRUE(f1);
    HU_ASSERT_TRUE(a.found);
    HU_ASSERT_FALSE(b.found); /* the rewrite was picked up, not the cached first version */
    (void)f2;
    HU_ASSERT_FALSE(f3);
    HU_ASSERT_FALSE(c.found);
}

void run_director_tapback_tests(void) {
    HU_TEST_SUITE("director_tapback");
    HU_RUN_TEST(tapback_shape_matches_586_vectors);
    HU_RUN_TEST(tapback_profile_reads_bucket_then_contact_then_global);
    HU_RUN_TEST(tapback_bucket_cutoff_is_n_weighted_over_bucket_cells);
    HU_RUN_TEST(tapback_contact_cutoff_comes_from_overall_cells);
    HU_RUN_TEST(tapback_global_lookup_has_no_cutoff);
    HU_RUN_TEST(tapback_all_zero_profile_overrides_every_tapback);
    HU_RUN_TEST(tapback_too_few_cells_is_no_data);
    HU_RUN_TEST(tapback_profile_without_reaction_fields_is_no_data);
    HU_RUN_TEST(tapback_check_overrides_mom_covid_news_from_her_story_rate);
    HU_RUN_TEST(tapback_check_overrides_ai_complaint_from_question_rate);
    HU_RUN_TEST(tapback_check_keeps_tapback_where_he_does_react);
    HU_RUN_TEST(tapback_check_without_data_keeps_the_model_decision);
    HU_RUN_TEST(tapback_check_leaves_text_decisions_alone);
    HU_RUN_TEST(tapback_facts_state_his_numbers);
    HU_RUN_TEST(tapback_profile_load_caches_by_file_version);
}
