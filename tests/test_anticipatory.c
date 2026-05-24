/* tests/test_anticipatory.c
 *
 * Sprint B Story 7 — anticipatory memory surfacing.
 * Contracts (8 tests; mirrors test_emotional_context shape):
 *   1. empty model → empty
 *   2. matching contact + lexicon + recent → renders UPCOMING
 *   3. older than 14d → dropped
 *   4. low confidence → dropped
 *   5. lexicon word-boundary safety ("birthdays" doesn't match "birthday")
 *   6. multiple matching → most recent wins
 *   7. NULL/empty args → empty
 *   8. now=0 in HU_IS_TEST → empty (deterministic)
 *
 * Note: a prior file with the same name was a no-op stub
 * (`typedef int hu_test_anticipatory_unused_;`) — this rewrite is the
 * first real coverage of the anticipatory module. The previous CMake
 * entry (if any) registered against the SQLite-gated stub; both the
 * source path and the test now live under the same module gate (no
 * gate — pure functions compile everywhere). */

#include "human/memory/anticipatory.h"
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
}

static void test_empty_model_writes_nothing(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_anticipatory_for_contact(&m, "alice", 1000000000, 0, buf, sizeof(buf)), 0);
}

static void test_matching_contact_lexicon_recent_renders(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    seed_fact(&m, "alice", "her", "birthday is", "next week", 0.8f, now - 3600);
    char buf[256] = {0};
    size_t n = hu_anticipatory_for_contact(&m, "alice", now, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "UPCOMING:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "birthday") != NULL);
}

static void test_older_than_lookback_dropped(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    int64_t old = now - HU_ANTICIPATORY_LOOKBACK_DEFAULT_SEC - 60; /* >14d */
    seed_fact(&m, "alice", "her", "wedding is", "in june", 0.8f, old);
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_anticipatory_for_contact(&m, "alice", now, 0, buf, sizeof(buf)), 0);
}

static void test_low_confidence_dropped(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    seed_fact(&m, "alice", "her", "trip is", "soon", 0.2f, now - 60);
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_anticipatory_for_contact(&m, "alice", now, 0, buf, sizeof(buf)), 0);
}

static void test_birthdays_does_not_match_birthday(void) {
    /* Lexicon has "birthday"; "birthdays" (with trailing 's') is NOT a
     * word-boundary match. Sanity check the word-boundary discipline
     * holds (matches substring-classifier-pitfalls rule). */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    seed_fact(&m, "alice", "the", "topic was", "birthdays", 0.9f, now - 60);
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_anticipatory_for_contact(&m, "alice", now, 0, buf, sizeof(buf)), 0);
}

static void test_multiple_matches_most_recent_wins(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000000;
    seed_fact(&m, "alice", "her", "exam is", "soon", 0.9f, now - 86400 * 5); /* 5d ago */
    seed_fact(&m, "alice", "her", "flight is", "tonight", 0.9f, now - 3600); /* 1h ago */
    char buf[256] = {0};
    size_t n = hu_anticipatory_for_contact(&m, "alice", now, 0, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "flight") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "exam") == NULL);
}

static void test_null_args_return_zero(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_anticipatory_for_contact(NULL, "alice", 1000000000, 0, buf, sizeof(buf)),
                 0);
    HU_ASSERT_EQ((int)hu_anticipatory_for_contact(&m, NULL, 1000000000, 0, buf, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_anticipatory_for_contact(&m, "", 1000000000, 0, buf, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_anticipatory_for_contact(&m, "alice", 1000000000, 0, NULL, 100), 0);
    HU_ASSERT_EQ((int)hu_anticipatory_for_contact(&m, "alice", 1000000000, 0, buf, 0), 0);
}

static void test_now_zero_returns_empty_in_test_mode(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_fact(&m, "alice", "her", "birthday is", "tomorrow", 0.8f, 1000000000);
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_anticipatory_for_contact(&m, "alice", 0, 0, buf, sizeof(buf)), 0);
}

void run_anticipatory_tests(void) {
    HU_TEST_SUITE("anticipatory");
    HU_RUN_TEST(test_empty_model_writes_nothing);
    HU_RUN_TEST(test_matching_contact_lexicon_recent_renders);
    HU_RUN_TEST(test_older_than_lookback_dropped);
    HU_RUN_TEST(test_low_confidence_dropped);
    HU_RUN_TEST(test_birthdays_does_not_match_birthday);
    HU_RUN_TEST(test_multiple_matches_most_recent_wins);
    HU_RUN_TEST(test_null_args_return_zero);
    HU_RUN_TEST(test_now_zero_returns_empty_in_test_mode);
}
