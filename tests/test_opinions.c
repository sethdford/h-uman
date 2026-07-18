typedef int hu_test_opinions_unused_;

/* ── Opinion-challenge detection (pure — runs in ALL build variants) ────── */

#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/memory/opinion_challenge.h"
#include "test_framework.h"
#include <string.h>

static void test_challenge_fires_on_topic_plus_disagreement(void) {
    const char *in1 = "nah pizza is overrated";
    HU_ASSERT_TRUE(hu_opinion_challenge_detect(in1, strlen(in1), "pizza", 5));
    const char *in2 = "you're wrong about remote work";
    HU_ASSERT_TRUE(hu_opinion_challenge_detect(in2, strlen(in2), "remote work", 11));
    const char *in3 = "I disagree, tabs are worse";
    HU_ASSERT_TRUE(hu_opinion_challenge_detect(in3, strlen(in3), "tabs", 4));
    const char *in4 = "no way, pizza every week is too much";
    HU_ASSERT_TRUE(hu_opinion_challenge_detect(in4, strlen(in4), "pizza", 5));
}

static void test_challenge_agreement_does_not_fire(void) {
    const char *in = "yeah pizza is honestly the best";
    HU_ASSERT_FALSE(hu_opinion_challenge_detect(in, strlen(in), "pizza", 5));
    const char *in2 = "totally agree about remote work";
    HU_ASSERT_FALSE(hu_opinion_challenge_detect(in2, strlen(in2), "remote work", 11));
}

static void test_challenge_unrelated_topic_does_not_fire(void) {
    /* Disagreement marker present but the stance topic is never referenced. */
    const char *in = "nah I really don't like this weather";
    HU_ASSERT_FALSE(hu_opinion_challenge_detect(in, strlen(in), "pizza", 5));
    const char *in2 = "that's just wrong dude";
    HU_ASSERT_FALSE(hu_opinion_challenge_detect(in2, strlen(in2), "remote work", 11));
}

static void test_challenge_substring_traps_do_not_fire(void) {
    /* "informal" must NOT match topic word "formal" (word-boundary rule). */
    const char *in1 = "the party was informal, no way I'm dressing up";
    HU_ASSERT_FALSE(hu_opinion_challenge_detect(in1, strlen(in1), "formal attire", 13));
    /* "wrongfully" must NOT fire the "wrong" marker. */
    const char *in2 = "he was wrongfully blamed for the pizza order";
    HU_ASSERT_FALSE(hu_opinion_challenge_detect(in2, strlen(in2), "pizza", 5));
    /* "disagreement" must NOT fire the "disagree" marker. */
    const char *in3 = "our disagreement about pizza was silly";
    HU_ASSERT_FALSE(hu_opinion_challenge_detect(in3, strlen(in3), "pizza", 5));
    /* Word-boundary DOES match across punctuation: "pizza?" is still pizza. */
    const char *in4 = "nah, pizza? not for me";
    HU_ASSERT_TRUE(hu_opinion_challenge_detect(in4, strlen(in4), "pizza", 5));
}

static void test_challenge_really_question_marker(void) {
    /* "really?" (skeptical) fires; bare "really" (intensifier) does not. */
    const char *in1 = "really? tabs?";
    HU_ASSERT_TRUE(hu_opinion_challenge_detect(in1, strlen(in1), "tabs", 4));
    const char *in2 = "tabs are really great";
    HU_ASSERT_FALSE(hu_opinion_challenge_detect(in2, strlen(in2), "tabs", 4));
    const char *in3 = "pizza is best, you think?";
    HU_ASSERT_TRUE(hu_opinion_challenge_detect(in3, strlen(in3), "pizza", 5));
}

static void test_challenge_short_and_stopword_topic_words_skipped(void) {
    /* Topic words under 3 chars and stopwords carry no signal — a topic of
     * "the ai" has no usable keyword, so even a marker-bearing inbound that
     * contains "the" must not fire. */
    const char *in = "nah the weather is bad";
    HU_ASSERT_FALSE(hu_opinion_challenge_detect(in, strlen(in), "the ai", 6));
}

static void test_challenge_null_and_empty_safe(void) {
    HU_ASSERT_FALSE(hu_opinion_challenge_detect(NULL, 0, "pizza", 5));
    HU_ASSERT_FALSE(hu_opinion_challenge_detect("nah pizza", 9, NULL, 0));
    HU_ASSERT_FALSE(hu_opinion_challenge_detect("", 0, "pizza", 5));
    HU_ASSERT_FALSE(hu_opinion_challenge_detect("nah pizza", 9, "", 0));
}

static void test_challenge_directive_off_is_inert(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "nah pizza is overrated";
    char *out = (char *)0x1;
    size_t out_len = 99;
    bool would = true;
    hu_error_t err = hu_opinion_challenge_directive(&alloc, HU_GATE_OFF, in, strlen(in), "pizza", 5,
                                                    "best food ever", 14, &out, &out_len, &would);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);
    HU_ASSERT_FALSE(would);
}

static void test_challenge_directive_shadow_flags_but_no_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "nah pizza is overrated";
    char *out = NULL;
    size_t out_len = 0;
    bool would = false;
    hu_error_t err =
        hu_opinion_challenge_directive(&alloc, HU_GATE_SHADOW, in, strlen(in), "pizza", 5,
                                       "best food ever", 14, &out, &out_len, &would);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_TRUE(would);
}

static void test_challenge_directive_live_fired_builds_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "nah pizza is overrated";
    char *out = NULL;
    size_t out_len = 0;
    bool would = false;
    hu_error_t err =
        hu_opinion_challenge_directive(&alloc, HU_GATE_LIVE, in, strlen(in), "pizza", 5,
                                       "best food ever", 14, &out, &out_len, &would);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(would);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_GT(out_len, 0);
    HU_ASSERT_STR_CONTAINS(out, "pushing back");
    HU_ASSERT_STR_CONTAINS(out, "pizza");
    HU_ASSERT_STR_CONTAINS(out, "best food ever");
    HU_ASSERT_STR_CONTAINS(out, "without abandoning");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_challenge_directive_live_no_challenge_stays_silent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "yeah pizza is honestly the best";
    char *out = NULL;
    size_t out_len = 0;
    bool would = true;
    hu_error_t err =
        hu_opinion_challenge_directive(&alloc, HU_GATE_LIVE, in, strlen(in), "pizza", 5,
                                       "best food ever", 14, &out, &out_len, &would);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_FALSE(would);
}

static void run_opinion_challenge_tests(void) {
    HU_RUN_TEST(test_challenge_fires_on_topic_plus_disagreement);
    HU_RUN_TEST(test_challenge_agreement_does_not_fire);
    HU_RUN_TEST(test_challenge_unrelated_topic_does_not_fire);
    HU_RUN_TEST(test_challenge_substring_traps_do_not_fire);
    HU_RUN_TEST(test_challenge_really_question_marker);
    HU_RUN_TEST(test_challenge_short_and_stopword_topic_words_skipped);
    HU_RUN_TEST(test_challenge_null_and_empty_safe);
    HU_RUN_TEST(test_challenge_directive_off_is_inert);
    HU_RUN_TEST(test_challenge_directive_shadow_flags_but_no_text);
    HU_RUN_TEST(test_challenge_directive_live_fired_builds_text);
    HU_RUN_TEST(test_challenge_directive_live_no_challenge_stays_silent);
}

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/humanness.h" /* hu_evolved_opinion_t + build_directive (firmness map) */
#include "human/memory.h"
#include "human/memory/opinions.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

static void test_opinions_upsert_get_pizza_best_food(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    int64_t now = (int64_t)time(NULL);
    hu_error_t err = hu_opinions_upsert(&alloc, &mem, "pizza", 5, "best food", 9, 0.8f, now);
    HU_ASSERT_EQ(err, HU_OK);

    hu_opinion_t *ops = NULL;
    size_t count = 0;
    err = hu_opinions_get(&alloc, &mem, "pizza", 5, &ops, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_NOT_NULL(ops[0].topic);
    HU_ASSERT_STR_EQ(ops[0].topic, "pizza");
    HU_ASSERT_STR_EQ(ops[0].position, "best food");
    HU_ASSERT_EQ(ops[0].superseded_by, 0);

    hu_opinions_free(&alloc, ops, count);
    mem.vtable->deinit(mem.ctx);
}

static void test_opinions_upsert_supersede_pizza_overrated(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_opinions_upsert(&alloc, &mem, "pizza", 5, "best food", 9, 0.8f, now), HU_OK);

    now += 100;
    HU_ASSERT_EQ(hu_opinions_upsert(&alloc, &mem, "pizza", 5, "overrated", 9, 0.6f, now), HU_OK);

    hu_opinion_t *ops = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_opinions_get(&alloc, &mem, "pizza", 5, &ops, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(ops[0].position, "overrated");
    hu_opinions_free(&alloc, ops, count);

    ops = NULL;
    count = 0;
    HU_ASSERT_EQ(hu_opinions_get_superseded(&alloc, &mem, "pizza", 5, &ops, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(ops[0].position, "best food");
    HU_ASSERT_NEQ(ops[0].superseded_by, 0);
    hu_opinions_free(&alloc, ops, count);

    mem.vtable->deinit(mem.ctx);
}

static void test_opinions_is_core_value_family(void) {
    const char *core_values[] = {"family", "honesty", "integrity"};
    HU_ASSERT_TRUE(hu_opinions_is_core_value("family", 6, core_values, 3));
    HU_ASSERT_TRUE(hu_opinions_is_core_value("Family", 6, core_values, 3));
    HU_ASSERT_TRUE(hu_opinions_is_core_value("HONESTY", 7, core_values, 3));
    HU_ASSERT_FALSE(hu_opinions_is_core_value("pizza", 5, core_values, 3));
    HU_ASSERT_FALSE(hu_opinions_is_core_value("fam", 3, core_values, 3));
}

/* A1 conviction loop AC-6: regression guard on the conviction->firmness
 * wording in hu_evolved_opinion_build_directive. The pre-generation stance
 * injection (agent_turn.c:2698) relies on this mapping; pin it so a future
 * edit can't silently flatten "firmly/moderately/tentatively". */
static void test_evolved_opinion_directive_firmness_mapping(void) {
    hu_allocator_t alloc = hu_system_allocator();

    char t_firm[] = "remote work", s_firm[] = "net positive";
    char t_mod[] = "tabs vs spaces", s_mod[] = "tabs win";
    char t_tent[] = "best pizza", s_tent[] = "thin crust";

    hu_evolved_opinion_t ops[3] = {
        {t_firm, strlen(t_firm), s_firm, strlen(s_firm), 0.9, 0, 7}, /* > 0.8 -> firmly */
        {t_mod, strlen(t_mod), s_mod, strlen(s_mod), 0.6, 0, 4},     /* > 0.5 -> moderately */
        {t_tent, strlen(t_tent), s_tent, strlen(s_tent), 0.3, 0, 2}, /* else -> tentatively */
    };

    size_t len = 0;
    char *dir = hu_evolved_opinion_build_directive(&alloc, ops, 3, 0.0, &len);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_GT(len, 0);
    HU_ASSERT_STR_CONTAINS(dir, "firmly");
    HU_ASSERT_STR_CONTAINS(dir, "moderately");
    HU_ASSERT_STR_CONTAINS(dir, "tentatively");
    alloc.free(alloc.ctx, dir, len + 1);
}

void run_opinions_tests(void) {
    HU_TEST_SUITE("opinions");
    HU_RUN_TEST(test_opinions_upsert_get_pizza_best_food);
    HU_RUN_TEST(test_opinions_upsert_supersede_pizza_overrated);
    HU_RUN_TEST(test_opinions_is_core_value_family);
    HU_RUN_TEST(test_evolved_opinion_directive_firmness_mapping);
    run_opinion_challenge_tests();
}

#else

void run_opinions_tests(void) {
    HU_TEST_SUITE("opinions");
    run_opinion_challenge_tests();
}

#endif /* HU_ENABLE_SQLITE */
