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
    hu_name_ref_t known[] = {{"Al", 2, false}, {"tampa", 5, false}};
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("Also that", known, 2, c, 8), 0L); /* "Al" is not in "Also" */
    HU_ASSERT_EQ((long)extract("saw al back in TAMPA", known, 2, c, 8), 2L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Al", HU_NAME_KNOWN));
    HU_ASSERT_TRUE(cand_is(&c[1], "tampa", HU_NAME_KNOWN)); /* the stored spelling */
}

static void test_extract_known_wins_over_capitalized_duplicate(void) {
    hu_name_ref_t known[] = {{"Salim", 5, false}};
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

/* I1: a candidate never ends in an apostrophe (closing quote, plural possessive). */
static void test_extract_strips_trailing_apostrophe_and_possessive(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("she said 'call Priya'", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Priya", HU_NAME_CAPITALIZED));
    HU_ASSERT_EQ((long)extract("that is Chris' car", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Chris", HU_NAME_CAPITALIZED));
    HU_ASSERT_EQ((long)extract("at Salim's place", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Salim", HU_NAME_CAPITALIZED));
}

/* I2: U+2019 (iOS smart apostrophe) behaves like '; emoji and U+FFFC are
 * boundaries, not letters, so a name touching them is still a candidate. */
static void test_extract_curly_apostrophe_and_emoji_neighbours(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("dinner at Priya\xe2\x80\x99s place", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Priya", HU_NAME_CAPITALIZED));
    HU_ASSERT_EQ((long)extract("that is Chris\xe2\x80\x99 car", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Chris", HU_NAME_CAPITALIZED));
    HU_ASSERT_EQ((long)extract("lunch with Priya\xf0\x9f\x98\x82 tonight", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Priya", HU_NAME_CAPITALIZED));
    HU_ASSERT_EQ((long)extract("photo from Salim\xef\xbf\xbc", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Salim", HU_NAME_CAPITALIZED));
    /* exactly like ': an inner apostrophe keeps the token whole either way */
    HU_ASSERT_EQ((long)extract("met D'Angelo today", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("met D\xe2\x80\x99"
                               "Angelo today",
                               NULL, 0, c, 8),
                 0L);
    hu_name_ref_t known[] = {{"Priya", 5, false}};
    HU_ASSERT_EQ((long)extract("Priya\xe2\x80\x99s surgery went fine", known, 1, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Priya", HU_NAME_KNOWN));
}

/* I3(a): an emoji, U+FFFC or an ellipsis ends a sentence, so the word after
 * it is autocapitalized, not a name. */
static void test_extract_emoji_ellipsis_and_object_end_a_sentence(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("lol \xf0\x9f\x98\x82 Salim is here", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("that was fun\xe2\x80\xa6 Salim called", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("look \xef\xbf\xbc Salim sent this", NULL, 0, c, 8), 0L);
    /* a variation selector does not end a sentence by itself */
    HU_ASSERT_EQ((long)extract("ok \xef\xb8\x8f with Salim", NULL, 0, c, 8), 1L);
}

/* I3(b): capitalized filler is not a name. */
static void test_extract_filler_stoplist(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("ok Im going, Love you", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("omg, Happy Birthday!", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("well Anyway call me Tomorrow", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("lol Going to bed Just now", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("ha Congrats and Welcome back Wow", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("ok Ive Ill Id Also Maybe Sure Nice Cool Damn", NULL, 0, c, 8), 0L);
    /* a greeting opening the message leaves the name after it */
    HU_ASSERT_EQ((long)extract("Hey Salim, you around?", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Salim", HU_NAME_CAPITALIZED));
    /* a stopword inside a run voids it */
    HU_ASSERT_EQ((long)extract("saw Salim And Priya", NULL, 0, c, 8), 0L);
    /* a stopword next to a name leaves the name */
    HU_ASSERT_EQ((long)extract("lol Thanks Priya", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Priya", HU_NAME_CAPITALIZED));
}

/* I3(c): an exact_case ref (an unconfirmed names:turn row) matches only its
 * own spelling; a normal ref matches any casing. */
static void test_extract_exact_case_ref_ignores_other_casing(void) {
    hu_name_candidate_t c[8];
    hu_name_ref_t exact[] = {{"Priya", 5, true}};
    HU_ASSERT_EQ((long)extract("saw priya today", exact, 1, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("saw PRIYA today", exact, 1, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("saw Priya today", exact, 1, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Priya", HU_NAME_KNOWN));
    hu_name_ref_t loose[] = {{"Priya", 5, false}};
    HU_ASSERT_EQ((long)extract("saw priya today", loose, 1, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Priya", HU_NAME_KNOWN));
}

/* M1: a non-ASCII letter is part of the word for KNOWN matching. */
static void test_extract_known_does_not_match_inside_non_ascii_word(void) {
    hu_name_candidate_t c[8];
    hu_name_ref_t known[] = {{"Ren", 3, false}};
    HU_ASSERT_EQ((long)extract("met Ren\xc3\xa9 today", known, 1, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("met \xc3\xa9Ren today", known, 1, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("met ren today", known, 1, c, 8), 1L);
}

/* M2: a hyphen joins two Capitalized parts into one name, not a lowercase one. */
static void test_extract_hyphenated_capitalized_name_is_one_token(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("met Mary-Kate today", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Mary-Kate", HU_NAME_CAPITALIZED));
    HU_ASSERT_EQ((long)extract("met Mary-kate today", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Mary", HU_NAME_CAPITALIZED));
    HU_ASSERT_EQ((long)extract("a well-Known band", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Known", HU_NAME_CAPITALIZED));
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
    HU_RUN_TEST(test_extract_strips_trailing_apostrophe_and_possessive);
    HU_RUN_TEST(test_extract_curly_apostrophe_and_emoji_neighbours);
    HU_RUN_TEST(test_extract_emoji_ellipsis_and_object_end_a_sentence);
    HU_RUN_TEST(test_extract_filler_stoplist);
    HU_RUN_TEST(test_extract_exact_case_ref_ignores_other_casing);
    HU_RUN_TEST(test_extract_known_does_not_match_inside_non_ascii_word);
    HU_RUN_TEST(test_extract_hyphenated_capitalized_name_is_one_token);
}
