#include "human/agent/salience.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <string.h>

/* P3: source -> category classification (word-boundary, per
 * substring-classifier-pitfalls.md). */
static void salience_classify_maps_sources_to_categories(void) {
    HU_ASSERT_TRUE(hu_salience_classify_source("emotional_checkin", 17) & HU_DIRECTIVE_EMOTIONAL);
    HU_ASSERT_TRUE(hu_salience_classify_source("emotional_checkin", 17) & HU_DIRECTIVE_PROACTIVE);
    HU_ASSERT_TRUE(hu_salience_classify_source("shared_reference", 16) & HU_DIRECTIVE_MEMORY);
    HU_ASSERT_TRUE(hu_salience_classify_source("somatic", 7) & HU_DIRECTIVE_IDENTITY);
    HU_ASSERT_TRUE(hu_salience_classify_source("safety_note", 11) & HU_DIRECTIVE_SAFETY);
    /* no keyword -> BEHAVIORAL default */
    HU_ASSERT_EQ(hu_salience_classify_source("inside_joke", 11), (uint32_t)HU_DIRECTIVE_BEHAVIORAL);
    HU_ASSERT_EQ(hu_salience_classify_source(NULL, 0), (uint32_t)HU_DIRECTIVE_BEHAVIORAL);
}

/* Never-suppress floor + word-boundary discipline. */
static void salience_required_floor_respects_word_boundary(void) {
    HU_ASSERT_TRUE(hu_salience_source_is_required("grief_support", 13));
    HU_ASSERT_TRUE(hu_salience_source_is_required("direct_question", 15));
    HU_ASSERT_TRUE(hu_salience_source_is_required("safety_check", 12));
    HU_ASSERT_FALSE(hu_salience_source_is_required("curiosity", 9));
    /* "questionnaire" contains "question" but NOT as a word -> must NOT be required */
    HU_ASSERT_FALSE(hu_salience_source_is_required("questionnaire", 13));
}

/* P2: profile weights. */
static void salience_profile_default_weights(void) {
    hu_salience_profile_t p;
    hu_salience_profile_init_default(&p);
    HU_ASSERT_FLOAT_EQ(hu_salience_profile_weight(&p, "shared_reference", 16), 1.6, 0.001);
    HU_ASSERT_FLOAT_EQ(hu_salience_profile_weight(&p, "curiosity", 9), 0.5, 0.001);
    /* unmatched source -> default weight 1.0 */
    HU_ASSERT_FLOAT_EQ(hu_salience_profile_weight(&p, "inside_joke", 11), 1.0, 0.001);
    /* NULL profile -> 1.0 */
    HU_ASSERT_FLOAT_EQ(hu_salience_profile_weight(NULL, "anything", 8), 1.0, 0.001);
}

/* P3: candidate construction. */
static void salience_build_candidate_fills_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_directive_t d;
    HU_ASSERT_EQ(hu_salience_build_candidate(&alloc, "grief_support", 13,
                                             "be gentle, they lost their dog", 30, &d),
                 HU_OK);
    HU_ASSERT_NOT_NULL(d.source);
    HU_ASSERT_NOT_NULL(d.content);
    HU_ASSERT_TRUE(d.required); /* grief -> never suppress */
    HU_ASSERT_TRUE(d.category & HU_DIRECTIVE_EMOTIONAL);
    HU_ASSERT_TRUE(d.token_cost > 0);
    hu_directive_deinit(&alloc, &d);
}

/* P4: shadow rank — required bypass + profile-weighted top-k, low-weight suppressed. */
static void salience_rank_keeps_required_and_top_weighted(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_salience_profile_t p;
    hu_salience_profile_init_default(&p);

    hu_directive_t c[3];
    /* grief is required (kept regardless); shared_reference high weight (1.6);
     * somatic low weight (0.4) should lose with a 2-directive budget. */
    HU_ASSERT_EQ(hu_salience_build_candidate(&alloc, "grief_support", 13, "gentle", 6, &c[0]),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_salience_build_candidate(&alloc, "shared_reference", 16, "the ramen place", 15, &c[1]),
        HU_OK);
    HU_ASSERT_EQ(hu_salience_build_candidate(&alloc, "somatic", 7, "you feel tired", 14, &c[2]),
                 HU_OK);

    hu_arbitration_config_t cfg = {.max_directive_tokens = 1500, .max_directives = 2};
    hu_arbitration_result_t res;
    HU_ASSERT_EQ(hu_salience_rank(&alloc, c, 3, &p, &cfg, &res), HU_OK);

    /* required grief must be selected; somatic (low weight) must be suppressed */
    bool kept_grief = false, kept_somatic = false;
    for (size_t i = 0; i < res.selected_count; i++) {
        if (strncmp(res.selected[i].source, "grief_support", res.selected[i].source_len) == 0)
            kept_grief = true;
        if (strncmp(res.selected[i].source, "somatic", res.selected[i].source_len) == 0)
            kept_somatic = true;
    }
    HU_ASSERT_TRUE(kept_grief);
    HU_ASSERT_FALSE(kept_somatic);
    HU_ASSERT_TRUE(res.suppressed_count >= 1);

    char *summary = hu_salience_summarize(&alloc, c, 3, &res);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(strstr(summary, "kept") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "suppressed") != NULL);
    alloc.free(alloc.ctx, summary, strlen(summary) + 1);

    hu_arbitration_result_deinit(&alloc, &res);
    for (size_t i = 0; i < 3; i++)
        hu_directive_deinit(&alloc, &c[i]);
}

void run_salience_tests(void) {
    HU_TEST_SUITE("salience");
    HU_RUN_TEST(salience_classify_maps_sources_to_categories);
    HU_RUN_TEST(salience_required_floor_respects_word_boundary);
    HU_RUN_TEST(salience_profile_default_weights);
    HU_RUN_TEST(salience_build_candidate_fills_fields);
    HU_RUN_TEST(salience_rank_keeps_required_and_top_weighted);
}
