/*
 * Reaction-signature calibration tests.
 *
 * Cover `hu_calib_reaction_signature_from_model`, `hu_calib_reactions_append_json`,
 * and `hu_calibrate_with_model` — the reactor-pattern summary surface that
 * lets the calibrated persona adapt per-contact based on facts ingested
 * by the reaction pipeline (`source_hint = "reaction_ingest"`).
 */

#include "human/calibration.h"
#include "human/core/string.h"
#include "human/memory/personal_model.h"
#include "test_framework.h"
#include <string.h>

/* Helper: append a single reaction-ingest fact to the model's facts[] array,
 * bypassing the trust / quarantine path so each test stays focused on the
 * calibration consumer. Returns false when the model is full. */
static bool add_reaction_fact(hu_personal_model_t *m, const char *subject, const char *predicate,
                              const char *object, int64_t last_seen) {
    if (m->fact_count >= HU_PM_MAX_FACTS)
        return false;
    hu_heuristic_fact_t *f = &m->facts[m->fact_count++];
    memset(f, 0, sizeof(*f));
    f->type = HU_KNOWLEDGE_PROPOSITIONAL;
    strncpy(f->subject, subject, sizeof(f->subject) - 1);
    strncpy(f->predicate, predicate, sizeof(f->predicate) - 1);
    strncpy(f->object, object, sizeof(f->object) - 1);
    strncpy(f->source_hint, "reaction_ingest", sizeof(f->source_hint) - 1);
    f->confidence = 0.8f;
    f->last_seen_at = last_seen;
    return true;
}

static void test_calibration_reactions_empty_model_yields_zero(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    hu_calib_reaction_signature_t sig;
    size_t n = hu_calib_reaction_signature_from_model(&m, &sig);
    HU_ASSERT_EQ(n, (size_t)0);
    HU_ASSERT_EQ(sig.reactor_count, (size_t)0);
    HU_ASSERT_EQ(sig.salient_topic_count, (size_t)0);
}

static void test_calibration_reactions_null_inputs_zero(void) {
    hu_calib_reaction_signature_t sig;
    memset(&sig, 0xAB, sizeof(sig));
    size_t n = hu_calib_reaction_signature_from_model(NULL, &sig);
    HU_ASSERT_EQ(n, (size_t)0);
    /* `out` is zero-initialised even on NULL model. */
    HU_ASSERT_EQ(sig.reactor_count, (size_t)0);
    HU_ASSERT_EQ(sig.salient_topic_count, (size_t)0);

    HU_ASSERT_EQ(hu_calib_reaction_signature_from_model(NULL, NULL), (size_t)0);
}

static void
test_calibration_reactions_three_loves_from_alice_on_hiking_yields_positive_reactor_and_topic(
    void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    add_reaction_fact(&m, "Alice", "reacted_with_love_to", "hiking saturday", 1700000000);
    add_reaction_fact(&m, "Alice", "reacted_with_love_to", "the hiking trail", 1700001000);
    add_reaction_fact(&m, "Alice", "reacted_with_love_to", "hiking weekend plans", 1700002000);

    hu_calib_reaction_signature_t sig;
    size_t n = hu_calib_reaction_signature_from_model(&m, &sig);
    HU_ASSERT_EQ(n, (size_t)1);
    HU_ASSERT_EQ(sig.reactor_count, (size_t)1);
    HU_ASSERT_EQ(strcmp(sig.top_reactors[0].handle, "Alice"), 0);
    HU_ASSERT_EQ(sig.top_reactors[0].positive_count, 3u);
    HU_ASSERT_EQ(sig.top_reactors[0].negative_count, 0u);
    HU_ASSERT_EQ(sig.top_reactors[0].last_observed, (int64_t)1700002000);

    /* "hiking" should surface as a salient topic (appears 3x). */
    bool found_hiking = false;
    for (size_t i = 0; i < sig.salient_topic_count; i++) {
        if (strcmp(sig.salient_topics[i], "hiking") == 0)
            found_hiking = true;
    }
    HU_ASSERT_TRUE(found_hiking);
}

static void test_calibration_reactions_polarity_classification(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    add_reaction_fact(&m, "Alice", "reacted_with_love_to", "trip plans", 100);
    add_reaction_fact(&m, "Alice", "laughed_at", "that meeting", 200);
    add_reaction_fact(&m, "Bob", "reacted_with_dislike_to", "rainy weekend", 300);
    add_reaction_fact(&m, "Bob", "questioned", "deadline change", 400);

    hu_calib_reaction_signature_t sig;
    hu_calib_reaction_signature_from_model(&m, &sig);
    HU_ASSERT_EQ(sig.reactor_count, (size_t)2);

    /* Both reactors have count=2, tie-broken by last_observed desc → Bob first (400 > 200). */
    HU_ASSERT_EQ(strcmp(sig.top_reactors[0].handle, "Bob"), 0);
    HU_ASSERT_EQ(sig.top_reactors[0].positive_count, 0u);
    HU_ASSERT_EQ(sig.top_reactors[0].negative_count, 2u);

    HU_ASSERT_EQ(strcmp(sig.top_reactors[1].handle, "Alice"), 0);
    HU_ASSERT_EQ(sig.top_reactors[1].positive_count, 2u);
    HU_ASSERT_EQ(sig.top_reactors[1].negative_count, 0u);
}

static void test_calibration_reactions_ignores_non_reaction_source_hints(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* User-direct fact about preferences should NOT count as a reaction. */
    hu_heuristic_fact_t *f = &m.facts[m.fact_count++];
    memset(f, 0, sizeof(*f));
    strncpy(f->subject, "Alice", sizeof(f->subject) - 1);
    strncpy(f->predicate, "reacted_with_love_to", sizeof(f->predicate) - 1);
    strncpy(f->object, "kayaking", sizeof(f->object) - 1);
    strncpy(f->source_hint, "user_direct", sizeof(f->source_hint) - 1);
    f->confidence = 0.9f;
    f->last_seen_at = 1700000000;

    /* And a genuine reaction fact alongside it. */
    add_reaction_fact(&m, "Bob", "laughed_at", "the funny story", 1700001000);

    hu_calib_reaction_signature_t sig;
    hu_calib_reaction_signature_from_model(&m, &sig);
    HU_ASSERT_EQ(sig.reactor_count, (size_t)1);
    HU_ASSERT_EQ(strcmp(sig.top_reactors[0].handle, "Bob"), 0);
}

static void test_calibration_reactions_top_eight_cap_respected_with_ten_reactors(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* 10 distinct reactors with strictly increasing counts so the
     * top-8 cap surfaces the most-active ones. Reactor "C0" gets 1 fact,
     * "C9" gets 10, so C2..C9 should be the top 8 (sorted desc). */
    for (int i = 0; i < 10; i++) {
        char handle[16];
        snprintf(handle, sizeof(handle), "C%d", i);
        for (int j = 0; j <= i; j++) {
            int64_t ts = 100 + i * 100 + j;
            add_reaction_fact(&m, handle, "reacted_with_love_to", "topic", ts);
        }
    }
    HU_ASSERT_TRUE(m.fact_count >= 10);

    hu_calib_reaction_signature_t sig;
    hu_calib_reaction_signature_from_model(&m, &sig);
    HU_ASSERT_EQ(sig.reactor_count, (size_t)HU_CALIB_REACTION_TOP_REACTORS);
    /* First entry must be the busiest reactor C9. */
    HU_ASSERT_EQ(strcmp(sig.top_reactors[0].handle, "C9"), 0);
    HU_ASSERT_EQ(sig.top_reactors[0].positive_count, 10u);
    /* Last entry of the top-8 must be C2 (3 facts). */
    HU_ASSERT_EQ(strcmp(sig.top_reactors[HU_CALIB_REACTION_TOP_REACTORS - 1].handle, "C2"), 0);
}

static void test_calibration_with_model_emits_reactions_field_in_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    add_reaction_fact(&m, "Alice", "reacted_with_love_to", "hiking saturday", 1700000000);
    add_reaction_fact(&m, "Alice", "laughed_at", "the funny story", 1700001000);

    char *json = NULL;
    HU_ASSERT_EQ(hu_calibrate_with_model(&alloc, NULL, NULL, "imessage", &m, &json), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    /* Base calibration fields still present. */
    HU_ASSERT_TRUE(strstr(json, "\"recommended_overlay\"") != NULL);
    HU_ASSERT_TRUE(strstr(json, "\"channel\":\"imessage\"") != NULL);
    /* New reactions field is present and well-formed. */
    HU_ASSERT_TRUE(strstr(json, "\"reactions\"") != NULL);
    HU_ASSERT_TRUE(strstr(json, "\"top_reactors\"") != NULL);
    HU_ASSERT_TRUE(strstr(json, "\"contact\":\"Alice\"") != NULL);
    HU_ASSERT_TRUE(strstr(json, "\"positive\":2") != NULL);
    HU_ASSERT_TRUE(strstr(json, "\"salient_topics\"") != NULL);
    hu_str_free(&alloc, json);
}

static void test_calibration_with_model_null_model_passes_through(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *json = NULL;
    HU_ASSERT_EQ(hu_calibrate_with_model(&alloc, NULL, NULL, NULL, NULL, &json), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    /* No reactions field when there's no model. */
    HU_ASSERT_TRUE(strstr(json, "\"reactions\"") == NULL);
    hu_str_free(&alloc, json);
}

void run_calibration_reactions_tests(void);
void run_calibration_reactions_tests(void) {
    HU_TEST_SUITE("calibration_reactions");
    HU_RUN_TEST(test_calibration_reactions_empty_model_yields_zero);
    HU_RUN_TEST(test_calibration_reactions_null_inputs_zero);
    HU_RUN_TEST(
        test_calibration_reactions_three_loves_from_alice_on_hiking_yields_positive_reactor_and_topic);
    HU_RUN_TEST(test_calibration_reactions_polarity_classification);
    HU_RUN_TEST(test_calibration_reactions_ignores_non_reaction_source_hints);
    HU_RUN_TEST(test_calibration_reactions_top_eight_cap_respected_with_ten_reactors);
    HU_RUN_TEST(test_calibration_with_model_emits_reactions_field_in_json);
    HU_RUN_TEST(test_calibration_with_model_null_model_passes_through);
}
