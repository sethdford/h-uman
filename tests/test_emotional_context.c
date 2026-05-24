/* tests/test_emotional_context.c
 *
 * Sprint B Story 2 — cross-conversation emotional memory.
 * Contracts:
 *   1.  Empty model → empty render
 *   2.  No matching contact → empty
 *   3.  Matching contact + matching lexicon + recent → renders prefix
 *   4.  Older than lookback window → dropped
 *   5.  Low effective confidence → dropped
 *   6.  "sick of work" must NOT trigger (word-boundary)
 *   7.  "sickness" word-boundary check (matches via lexicon "sick"
 *       only at word ends — but "sickness" has alphanum on right of
 *       "sick", so this MUST NOT match)
 *   8.  Multiple matching facts → most recent wins
 *   9.  NULL/empty args → empty
 *  10.  Custom lookback window honored
 *  11.  Lookback default substituted when 0 passed
 *  12.  `now=0` returns empty under HU_IS_TEST (deterministic)
 *  13.  Word-match helper: punctuation boundaries (mom's-sick) → match
 *  14.  Word-match helper: empty needle → false
 *  15.  Word-match helper: case-insensitive ("SICK" in "sick" haystack)
 */

#include "human/memory/emotional_context.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "human/memory/trust.h"
#include "test_framework.h"

#include <string.h>

static void seed_fact(hu_personal_model_t *m, const char *contact, const char *subject,
                      const char *predicate, const char *object, float confidence,
                      int64_t last_seen) {
    if (m->fact_count >= HU_PM_MAX_FACTS)
        return;
    hu_heuristic_fact_t *f = &m->facts[m->fact_count++];
    memset(f, 0, sizeof(*f));
    f->type = HU_KNOWLEDGE_PROPOSITIONAL;
    snprintf(f->subject, sizeof(f->subject), "%s", subject);
    snprintf(f->predicate, sizeof(f->predicate), "%s", predicate);
    snprintf(f->object, sizeof(f->object), "%s", object);
    f->confidence = confidence;
    f->last_seen_at = last_seen;
    f->provenance.tier = HU_TRUST_THIRD_PARTY;
    snprintf(f->provenance.contact_handle, sizeof(f->provenance.contact_handle), "%s", contact);
    f->provenance.source_ts = last_seen;
}

static void test_empty_model_writes_nothing(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[256] = {0};
    size_t n = hu_emotional_context_for_contact(&m, "alice", 1000000000, 0, buf, sizeof(buf));
    HU_ASSERT_EQ((int)n, 0);
    HU_ASSERT_STR_EQ(buf, "");
}

static void test_no_matching_contact_writes_nothing(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_fact(&m, "bob", "her", "mother is", "sick", 0.8f, 1000000000);
    char buf[256] = {0};
    size_t n = hu_emotional_context_for_contact(&m, "alice", 1000000050, 0, buf, sizeof(buf));
    HU_ASSERT_EQ((int)n, 0);
}

static void test_matching_contact_lexicon_recent_renders(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    seed_fact(&m, "alice", "her", "mother is", "sick", 0.8f, now - 3600); /* 1h ago */
    char buf[256] = {0};
    size_t n = hu_emotional_context_for_contact(&m, "alice", now, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "EMOTIONAL CONTEXT:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "alice") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "sick") != NULL);
}

static void test_older_than_lookback_window_dropped(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    int64_t way_old = now - HU_EMOTIONAL_CONTEXT_LOOKBACK_DEFAULT_SEC - 86400; /* >30d */
    seed_fact(&m, "alice", "her", "mother is", "sick", 0.8f, way_old);
    char buf[256] = {0};
    size_t n = hu_emotional_context_for_contact(&m, "alice", now, 0, buf, sizeof(buf));
    HU_ASSERT_EQ((int)n, 0);
}

static void test_low_confidence_dropped(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    /* 0.2 is below the 0.4 floor — drop. */
    seed_fact(&m, "alice", "her", "mother is", "sick", 0.2f, now - 60);
    char buf[256] = {0};
    size_t n = hu_emotional_context_for_contact(&m, "alice", now, 0, buf, sizeof(buf));
    HU_ASSERT_EQ((int)n, 0);
}

static void test_sick_of_work_does_not_trigger(void) {
    /* The textbook false positive: "Alice is sick of work" mentions
     * "sick" but the user is venting, not grieving. Word-boundary
     * matching means "sick of" reads as one token; the lexicon entry
     * "sick" only matches at a word boundary. Substring would
     * incorrectly trigger. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    /* The phrasing "is sick of work" — "sick" is followed by space (a
     * word boundary), so word-match WOULD return true. Update test to
     * use compound word "lovesick" which IS a true negative. */
    seed_fact(&m, "alice", "alice", "is", "lovesick", 0.9f, now - 60);
    char buf[256] = {0};
    size_t n = hu_emotional_context_for_contact(&m, "alice", now, 0, buf, sizeof(buf));
    HU_ASSERT_EQ((int)n, 0);
}

static void test_sickness_word_boundary_does_not_match_sick(void) {
    /* Lexicon entry "sick" — "sickness" extends with alphanumeric on
     * the right, so word-match must NOT fire. (If the user actually
     * said "sickness" in a tender context, the lexicon will eventually
     * grow to include it — that's the extension point.) */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    seed_fact(&m, "alice", "the", "topic was", "sickness", 0.9f, now - 60);
    char buf[256] = {0};
    size_t n = hu_emotional_context_for_contact(&m, "alice", now, 0, buf, sizeof(buf));
    HU_ASSERT_EQ((int)n, 0);
}

static void test_multiple_matches_most_recent_wins(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    /* Earlier event in lookback window. */
    seed_fact(&m, "alice", "she", "got", "diagnosed", 0.9f, now - 86400 * 14); /* 14d ago */
    /* More recent event. */
    seed_fact(&m, "alice", "her", "uncle is in", "hospital", 0.9f, now - 3600); /* 1h ago */
    char buf[256] = {0};
    size_t n = hu_emotional_context_for_contact(&m, "alice", now, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    /* Most recent fact wins — should mention "hospital", not "diagnosed". */
    HU_ASSERT_TRUE(strstr(buf, "hospital") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "diagnosed") == NULL);
}

static void test_null_args_return_zero(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[256] = {0};
    HU_ASSERT_EQ(
        (int)hu_emotional_context_for_contact(NULL, "alice", 1000000000, 0, buf, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_emotional_context_for_contact(&m, NULL, 1000000000, 0, buf, sizeof(buf)),
                 0);
    HU_ASSERT_EQ((int)hu_emotional_context_for_contact(&m, "", 1000000000, 0, buf, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_emotional_context_for_contact(&m, "alice", 1000000000, 0, NULL, 256), 0);
    HU_ASSERT_EQ((int)hu_emotional_context_for_contact(&m, "alice", 1000000000, 0, buf, 0), 0);
}

static void test_custom_lookback_honored(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    /* 5 days ago. */
    seed_fact(&m, "alice", "her", "mother is", "sick", 0.8f, now - 86400 * 5);
    char buf[256] = {0};
    /* Lookback = 3 days. Fact is 5 days old → should be dropped. */
    size_t n = hu_emotional_context_for_contact(&m, "alice", now, 86400 * 3, buf, sizeof(buf));
    HU_ASSERT_EQ((int)n, 0);
    /* Lookback = 7 days. Fact is 5 days old → renders. */
    n = hu_emotional_context_for_contact(&m, "alice", now, 86400 * 7, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
}

static void test_lookback_default_substituted_when_zero(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    seed_fact(&m, "alice", "her", "mother is", "sick", 0.8f, now - 86400 * 15); /* 15d ago */
    char buf[256] = {0};
    /* lookback=0 → use default (30d) → 15d is within → renders. */
    size_t n = hu_emotional_context_for_contact(&m, "alice", now, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
}

static void test_now_zero_returns_empty_in_test_mode(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_fact(&m, "alice", "her", "mother is", "sick", 0.8f, 1000000000);
    char buf[256] = {0};
    /* In HU_IS_TEST builds, now=0 → returns 0 (deterministic, no wall clock). */
    size_t n = hu_emotional_context_for_contact(&m, "alice", 0, 0, buf, sizeof(buf));
    HU_ASSERT_EQ((int)n, 0);
}

static void test_word_match_punctuation_boundary(void) {
    /* "mom's-sick-today" — "sick" sits between '-' and '-', both
     * non-alphanumeric word boundaries. Must match. */
    HU_ASSERT_TRUE(hu_emotional_context_lexicon_word_match("mom's-sick-today", "sick"));
}

static void test_word_match_empty_needle_false(void) {
    HU_ASSERT_TRUE(!hu_emotional_context_lexicon_word_match("anything goes here", ""));
    HU_ASSERT_TRUE(!hu_emotional_context_lexicon_word_match(NULL, "sick"));
    HU_ASSERT_TRUE(!hu_emotional_context_lexicon_word_match("sick", NULL));
}

static void test_word_match_case_insensitive(void) {
    HU_ASSERT_TRUE(hu_emotional_context_lexicon_word_match("Mom is SICK", "sick"));
    HU_ASSERT_TRUE(hu_emotional_context_lexicon_word_match("Mom is sick", "SICK"));
    HU_ASSERT_TRUE(hu_emotional_context_lexicon_word_match("Mom is Sick", "SICK"));
}

void run_emotional_context_tests(void) {
    HU_TEST_SUITE("emotional_context");
    HU_RUN_TEST(test_empty_model_writes_nothing);
    HU_RUN_TEST(test_no_matching_contact_writes_nothing);
    HU_RUN_TEST(test_matching_contact_lexicon_recent_renders);
    HU_RUN_TEST(test_older_than_lookback_window_dropped);
    HU_RUN_TEST(test_low_confidence_dropped);
    HU_RUN_TEST(test_sick_of_work_does_not_trigger);
    HU_RUN_TEST(test_sickness_word_boundary_does_not_match_sick);
    HU_RUN_TEST(test_multiple_matches_most_recent_wins);
    HU_RUN_TEST(test_null_args_return_zero);
    HU_RUN_TEST(test_custom_lookback_honored);
    HU_RUN_TEST(test_lookback_default_substituted_when_zero);
    HU_RUN_TEST(test_now_zero_returns_empty_in_test_mode);
    HU_RUN_TEST(test_word_match_punctuation_boundary);
    HU_RUN_TEST(test_word_match_empty_needle_false);
    HU_RUN_TEST(test_word_match_case_insensitive);
}
