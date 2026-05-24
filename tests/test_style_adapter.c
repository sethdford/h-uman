/* tests/test_style_adapter.c
 *
 * Sprint B B-loop — per-contact style hint derived from
 * causal_attribution counts.
 * Contracts (10 tests):
 *   1. NULL/empty contact → UNKNOWN
 *   2. fewer than MIN_REACTIONS → UNKNOWN
 *   3. exactly MIN_REACTIONS, all positive → POSITIVE
 *   4. >80% positive AND >=5 total → VERY_POSITIVE
 *   5. >50% positive but ≤80% → POSITIVE
 *   6. >50% negative → NEGATIVE
 *   7. mixed neutral → NEUTRAL
 *   8. warmth_label maps each enum value to a non-NULL string
 *   9. render_hint UNKNOWN → empty
 *  10. render_hint populated → contains "STYLE HINT:" + handle + guidance
 */

#include "human/persona/style_adapter.h"

#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "human/memory/trust.h"
#include "test_framework.h"

#include <string.h>

static void seed_reaction(hu_personal_model_t *m, const char *contact, const char *predicate) {
    if (m->fact_count >= HU_PM_MAX_FACTS)
        return;
    hu_heuristic_fact_t *f = &m->facts[m->fact_count++];
    memset(f, 0, sizeof(*f));
    f->type = HU_KNOWLEDGE_PROPOSITIONAL;
    snprintf(f->subject, sizeof(f->subject), "%s", contact);
    snprintf(f->predicate, sizeof(f->predicate), "%s", predicate);
    snprintf(f->object, sizeof(f->object), "%s", "x");
    snprintf(f->source_hint, sizeof(f->source_hint), "reaction_ingest");
    f->confidence = 0.7f;
    f->last_seen_at = 1000;
    snprintf(f->provenance.contact_handle, sizeof(f->provenance.contact_handle), "%s", contact);
    f->provenance.tier = HU_TRUST_THIRD_PARTY;
}

static void test_null_contact_unknown(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_EQ((int)hu_style_adapter_warmth(&m, NULL), (int)HU_STYLE_WARMTH_UNKNOWN);
    HU_ASSERT_EQ((int)hu_style_adapter_warmth(NULL, "alice"), (int)HU_STYLE_WARMTH_UNKNOWN);
    HU_ASSERT_EQ((int)hu_style_adapter_warmth(&m, ""), (int)HU_STYLE_WARMTH_UNKNOWN);
}

static void test_fewer_than_min_reactions_unknown(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_reaction(&m, "alice", "loves");
    seed_reaction(&m, "alice", "loves");
    /* total = 2 < MIN_REACTIONS=3 → UNKNOWN. */
    HU_ASSERT_EQ((int)hu_style_adapter_warmth(&m, "alice"), (int)HU_STYLE_WARMTH_UNKNOWN);
}

static void test_min_reactions_all_positive_positive(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_reaction(&m, "alice", "loves");
    seed_reaction(&m, "alice", "likes");
    seed_reaction(&m, "alice", "appreciates");
    /* 3/3 positive, total=3 (<5) → POSITIVE (not VERY_POSITIVE). */
    HU_ASSERT_EQ((int)hu_style_adapter_warmth(&m, "alice"), (int)HU_STYLE_WARMTH_POSITIVE);
}

static void test_very_positive_threshold(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* 5 positive, 0 negative → 100% positive, total=5 → VERY_POSITIVE. */
    for (int i = 0; i < 5; i++)
        seed_reaction(&m, "alice", "loves");
    HU_ASSERT_EQ((int)hu_style_adapter_warmth(&m, "alice"), (int)HU_STYLE_WARMTH_VERY_POSITIVE);
}

static void test_positive_but_below_very_positive(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* 3 positive + 2 neutral = 5 total, 60% positive → POSITIVE (not VERY). */
    seed_reaction(&m, "alice", "loves");
    seed_reaction(&m, "alice", "likes");
    seed_reaction(&m, "alice", "appreciates");
    seed_reaction(&m, "alice", "mentioned");
    seed_reaction(&m, "alice", "noted");
    HU_ASSERT_EQ((int)hu_style_adapter_warmth(&m, "alice"), (int)HU_STYLE_WARMTH_POSITIVE);
}

static void test_negative_majority_negative(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_reaction(&m, "alice", "hates");
    seed_reaction(&m, "alice", "dislikes");
    seed_reaction(&m, "alice", "resents");
    /* 3/3 negative → NEGATIVE. */
    HU_ASSERT_EQ((int)hu_style_adapter_warmth(&m, "alice"), (int)HU_STYLE_WARMTH_NEGATIVE);
}

static void test_mixed_neutral_majority_neutral(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* 1 positive + 1 negative + 2 neutral = 4 total, neither >50% → NEUTRAL. */
    seed_reaction(&m, "alice", "loves");
    seed_reaction(&m, "alice", "hates");
    seed_reaction(&m, "alice", "mentioned");
    seed_reaction(&m, "alice", "noted");
    HU_ASSERT_EQ((int)hu_style_adapter_warmth(&m, "alice"), (int)HU_STYLE_WARMTH_NEUTRAL);
}

static void test_warmth_labels(void) {
    HU_ASSERT_STR_EQ(hu_style_adapter_warmth_label(HU_STYLE_WARMTH_UNKNOWN), "unknown");
    HU_ASSERT_STR_EQ(hu_style_adapter_warmth_label(HU_STYLE_WARMTH_NEGATIVE), "negative");
    HU_ASSERT_STR_EQ(hu_style_adapter_warmth_label(HU_STYLE_WARMTH_NEUTRAL), "neutral");
    HU_ASSERT_STR_EQ(hu_style_adapter_warmth_label(HU_STYLE_WARMTH_POSITIVE), "positive");
    HU_ASSERT_STR_EQ(hu_style_adapter_warmth_label(HU_STYLE_WARMTH_VERY_POSITIVE), "very_positive");
}

static void test_render_hint_unknown_returns_zero(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_style_adapter_render_hint(&m, "alice", buf, sizeof(buf)), 0);
}

static void test_render_hint_populated_contains_prefix_and_handle(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    for (int i = 0; i < 5; i++)
        seed_reaction(&m, "alice", "loves");
    char buf[256] = {0};
    size_t n = hu_style_adapter_render_hint(&m, "alice", buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "STYLE HINT:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "alice") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "very well") != NULL); /* very_positive guidance */
}

void run_style_adapter_tests(void) {
    HU_TEST_SUITE("style_adapter");
    HU_RUN_TEST(test_null_contact_unknown);
    HU_RUN_TEST(test_fewer_than_min_reactions_unknown);
    HU_RUN_TEST(test_min_reactions_all_positive_positive);
    HU_RUN_TEST(test_very_positive_threshold);
    HU_RUN_TEST(test_positive_but_below_very_positive);
    HU_RUN_TEST(test_negative_majority_negative);
    HU_RUN_TEST(test_mixed_neutral_majority_neutral);
    HU_RUN_TEST(test_warmth_labels);
    HU_RUN_TEST(test_render_hint_unknown_returns_zero);
    HU_RUN_TEST(test_render_hint_populated_contains_prefix_and_handle);
}
