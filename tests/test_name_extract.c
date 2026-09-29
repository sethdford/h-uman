/* The zero-model per-turn name catcher (spec 2026-09-29 §4.2): KNOWN names of
 * the contact matched at word boundaries, and new Capitalized 1-3 token runs
 * that are not at a sentence start and not stopwords. */
#include "human/memory/name_extract.h"
#include "test_framework.h"
#include <stdbool.h>
#include <string.h>

static size_t extract(const char *text, const hu_name_ref_t *known, size_t kn,
                      hu_name_candidate_t *out, size_t cap) {
    return hu_name_extract(text, strlen(text), known, kn, out, cap);
}

static bool cand_is(const hu_name_candidate_t *c, const char *name, hu_name_kind_t kind) {
    return c->len == strlen(name) && memcmp(c->name, name, c->len) == 0 && c->kind == kind;
}

static void test_nameable_truth_table(void) {
    HU_ASSERT_TRUE(hu_name_entity_is_nameable(HU_ENTITY_PERSON, "salim", 5));
    HU_ASSERT_TRUE(hu_name_entity_is_nameable(HU_ENTITY_PLACE, "tampa", 5));
    HU_ASSERT_TRUE(hu_name_entity_is_nameable(HU_ENTITY_ORGANIZATION, "Acme", 4));
    HU_ASSERT_TRUE(hu_name_entity_is_nameable(HU_ENTITY_EVENT, "Coachella", 9));
    HU_ASSERT_TRUE(hu_name_entity_is_nameable(HU_ENTITY_UNKNOWN, "Zed Corp", 8));
    HU_ASSERT_FALSE(hu_name_entity_is_nameable(HU_ENTITY_UNKNOWN, "different direction", 19));
    HU_ASSERT_FALSE(hu_name_entity_is_nameable(HU_ENTITY_TOPIC, "Pickleball", 10));
    HU_ASSERT_FALSE(hu_name_entity_is_nameable(HU_ENTITY_EMOTION, "Grief", 5));
    HU_ASSERT_FALSE(hu_name_entity_is_nameable(HU_ENTITY_PERSON, NULL, 0));
    HU_ASSERT_FALSE(hu_name_entity_is_nameable((hu_entity_type_t)42, "X", 1));
}

static void test_extract_capitalized_mid_sentence(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("had lunch with Salim yesterday", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Salim", HU_NAME_CAPITALIZED));
    HU_ASSERT_EQ((long)extract("went to Tampa Bay with Priya", NULL, 0, c, 8), 2L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Tampa Bay", HU_NAME_CAPITALIZED));
    HU_ASSERT_TRUE(cand_is(&c[1], "Priya", HU_NAME_CAPITALIZED));
    HU_ASSERT_EQ((long)extract("dinner at Priya's place", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Priya", HU_NAME_CAPITALIZED)); /* possessive stripped */
    HU_ASSERT_EQ((long)extract("I saw Jo", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Jo", HU_NAME_CAPITALIZED)); /* 2 chars is the floor */
}

static void test_extract_sentence_start_is_not_a_name(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("Salim came by", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("ok. Salim came by", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("wow!\nSalim came by", NULL, 0, c, 8), 0L);
}

static void test_extract_stoplist_but_mom_and_dad_allowed(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("see you Monday in March", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("thanks God lol Hey", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("call Mom later and Dad too", NULL, 0, c, 8), 2L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Mom", HU_NAME_CAPITALIZED));
    HU_ASSERT_TRUE(cand_is(&c[1], "Dad", HU_NAME_CAPITALIZED));
}

static void test_extract_known_names_word_boundary_and_lowercase(void) {
    hu_name_ref_t known[] = {{"Al", 2}, {"tampa", 5}};
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("Also that", known, 2, c, 8), 0L); /* "Al" is not in "Also" */
    HU_ASSERT_EQ((long)extract("saw al back in TAMPA", known, 2, c, 8), 2L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Al", HU_NAME_KNOWN));
    HU_ASSERT_TRUE(cand_is(&c[1], "tampa", HU_NAME_KNOWN)); /* the stored spelling */
}

static void test_extract_known_wins_over_capitalized_duplicate(void) {
    hu_name_ref_t known[] = {{"Salim", 5}};
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("lunch with Salim", known, 1, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Salim", HU_NAME_KNOWN));
}

static void test_extract_respects_out_cap_and_null_inputs(void) {
    hu_name_candidate_t c[2];
    HU_ASSERT_EQ((long)extract("saw Ann and Bob and Cal and Dee", NULL, 0, c, 2), 2L);
    HU_ASSERT_EQ((long)hu_name_extract(NULL, 5, NULL, 0, c, 2), 0L);
    HU_ASSERT_EQ((long)hu_name_extract("saw Ann", 7, NULL, 0, NULL, 2), 0L);
    HU_ASSERT_EQ((long)hu_name_extract("saw Ann", 7, NULL, 0, c, 0), 0L);
}

/* Review Focus 1: an ASCII fragment of a non-ASCII word is not a name. */
static void test_extract_non_ascii_glued_token_is_not_a_candidate(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("met Jos\xc3\xa9 today", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("going to Zo\xc3\xab's party", NULL, 0, c, 8), 0L);
}

/* Review Focus 5: all-caps is not Capitalized; a 4-token title run is not a name. */
static void test_extract_all_caps_and_long_title_runs_are_not_names(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("omg SALIM IS HERE", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("we said Happy New Year Everyone", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("at the NYC office", NULL, 0, c, 8), 0L);
}

void run_name_extract_tests(void) {
    HU_TEST_SUITE("name_extract");
    HU_RUN_TEST(test_nameable_truth_table);
    HU_RUN_TEST(test_extract_capitalized_mid_sentence);
    HU_RUN_TEST(test_extract_sentence_start_is_not_a_name);
    HU_RUN_TEST(test_extract_stoplist_but_mom_and_dad_allowed);
    HU_RUN_TEST(test_extract_known_names_word_boundary_and_lowercase);
    HU_RUN_TEST(test_extract_known_wins_over_capitalized_duplicate);
    HU_RUN_TEST(test_extract_respects_out_cap_and_null_inputs);
    HU_RUN_TEST(test_extract_non_ascii_glued_token_is_not_a_candidate);
    HU_RUN_TEST(test_extract_all_caps_and_long_title_runs_are_not_names);
}
