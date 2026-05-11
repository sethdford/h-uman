/* B11 — Unit tests for hu_personal_model_contradicts_user.
 *
 * Covers the two contradiction shapes documented in the header:
 *   (1) same subject + same predicate + different object
 *   (2) same subject + antonym predicate pair + same object
 * plus negative cases (no facts, low-confidence stored fact, near-synonym
 * predicate, empty objects) that must NOT produce false-positive
 * contradictions.
 *
 * The agent_turn wire is pinned end-to-end by
 * tests/test_b11_pressure_history_e2e.c (which already drives the
 * pressure-history side); this file pins the contradicts function in
 * isolation so a regression there is attributed cleanly. */

#include "human/core/allocator.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "test_framework.h"

#include <stdbool.h>
#include <string.h>

/* Helper: stamp one heuristic fact directly into the personal model.
 * Bypasses hu_personal_model_ingest so tests stay focused on the
 * contradicts logic — ingest's heuristic extraction is tested
 * separately. */
static void pm_stamp_fact(hu_personal_model_t *m, const char *subject, const char *predicate,
                          const char *object, float confidence) {
    HU_ASSERT_TRUE(m->fact_count < HU_PM_MAX_FACTS);
    hu_heuristic_fact_t *f = &m->facts[m->fact_count++];
    memset(f, 0, sizeof(*f));
    f->type = HU_KNOWLEDGE_PROPOSITIONAL;
    strncpy(f->subject, subject, sizeof(f->subject) - 1);
    strncpy(f->predicate, predicate, sizeof(f->predicate) - 1);
    strncpy(f->object, object, sizeof(f->object) - 1);
    f->confidence = confidence;
}

/* Shape 1: same predicate, different object → contradicts. */
static void contradicts_same_predicate_different_object(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    pm_stamp_fact(&m, "user", "i work at", "Acme", 0.9f);

    bool c = false;
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, "I work at Initech.", 18, &c), HU_OK);
    HU_ASSERT_TRUE(c);
}

/* Shape 1 negative: same predicate AND same object → no contradiction. */
static void contradicts_same_predicate_same_object_does_not_fire(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    pm_stamp_fact(&m, "user", "i work at", "Acme", 0.9f);

    bool c = true;
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, "I work at Acme.", 15, &c), HU_OK);
    HU_ASSERT_FALSE(c);
}

/* Shape 2: antonym predicates, same object → contradicts. */
static void contradicts_antonym_like_vs_hate(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    pm_stamp_fact(&m, "user", "i like", "coffee", 0.8f);

    bool c = false;
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, "I hate coffee.", 14, &c), HU_OK);
    HU_ASSERT_TRUE(c);
}

/* Shape 2 negative: near-synonyms (like vs love) must NOT contradict —
 * they're emphatic variants, not antonyms. */
static void contradicts_near_synonym_like_vs_love_does_not_fire(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    pm_stamp_fact(&m, "user", "i like", "coffee", 0.8f);

    bool c = true;
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, "I love coffee.", 14, &c), HU_OK);
    HU_ASSERT_FALSE(c);
}

/* Antonym pair on a different object → no contradiction.
 * "I like coffee" vs "I hate tea" should NOT fire because the objects
 * differ — they are independent claims about different things. */
static void contradicts_antonym_different_object_does_not_fire(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    pm_stamp_fact(&m, "user", "i like", "coffee", 0.8f);

    bool c = true;
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, "I hate tea.", 11, &c), HU_OK);
    HU_ASSERT_FALSE(c);
}

/* Low-confidence stored facts (< 0.6) must not produce push-back.
 * A half-remembered guess shouldn't gaslight the user. */
static void contradicts_low_confidence_stored_fact_is_ignored(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    pm_stamp_fact(&m, "user", "i work at", "Acme", 0.4f);

    bool c = true;
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, "I work at Initech.", 18, &c), HU_OK);
    HU_ASSERT_FALSE(c);
}

/* Empty model → no contradiction possible. */
static void contradicts_empty_model_returns_false(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    bool c = true;
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, "I love coffee.", 14, &c), HU_OK);
    HU_ASSERT_FALSE(c);
}

/* Empty message → no contradiction possible. */
static void contradicts_empty_message_returns_false(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    pm_stamp_fact(&m, "user", "i like", "coffee", 0.8f);

    bool c = true;
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, "", 0, &c), HU_OK);
    HU_ASSERT_FALSE(c);
}

/* NULL args — programming-error guard. */
static void contradicts_null_args_rejected(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    bool c = false;
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(NULL, "hi", 2, &c), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, NULL, 2, &c), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, "hi", 2, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* Multiple stored facts: contradiction with ANY one of them is enough. */
static void contradicts_finds_match_in_multiple_facts(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    pm_stamp_fact(&m, "user", "i live in", "Berlin", 0.9f);
    pm_stamp_fact(&m, "user", "i work at", "Acme", 0.9f);
    pm_stamp_fact(&m, "user", "i like", "coffee", 0.8f);

    bool c = false;
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, "I work at Initech.", 18, &c), HU_OK);
    HU_ASSERT_TRUE(c);
}

/* Realistic round-trip: ingest a message then assert a later
 * contradicting message fires the signal. Proves the function works
 * against facts created by the public ingest path, not just hand-stamped
 * ones. */
static void contradicts_after_ingest_round_trip(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "I live in Berlin.", 17, true, 1700000001), HU_OK);

    bool c = false;
    HU_ASSERT_EQ(hu_personal_model_contradicts_user(&m, "I live in Paris.", 16, &c), HU_OK);
    HU_ASSERT_TRUE(c);
}

void run_personal_model_contradicts_tests(void);

void run_personal_model_contradicts_tests(void) {
    HU_TEST_SUITE("personal_model contradicts");
    HU_RUN_TEST(contradicts_same_predicate_different_object);
    HU_RUN_TEST(contradicts_same_predicate_same_object_does_not_fire);
    HU_RUN_TEST(contradicts_antonym_like_vs_hate);
    HU_RUN_TEST(contradicts_near_synonym_like_vs_love_does_not_fire);
    HU_RUN_TEST(contradicts_antonym_different_object_does_not_fire);
    HU_RUN_TEST(contradicts_low_confidence_stored_fact_is_ignored);
    HU_RUN_TEST(contradicts_empty_model_returns_false);
    HU_RUN_TEST(contradicts_empty_message_returns_false);
    HU_RUN_TEST(contradicts_null_args_rejected);
    HU_RUN_TEST(contradicts_finds_match_in_multiple_facts);
    HU_RUN_TEST(contradicts_after_ingest_round_trip);
}
