/* tests/test_causal_attribution.c
 *
 * Sprint B Story 6 — causal attribution.
 * Contracts (8 tests):
 *   1. empty model → 0
 *   2. non-reaction-source facts ignored
 *   3. positive verb counts → positive_count++
 *   4. negative verb counts → negative_count++
 *   5. unknown predicate → neutral_count++
 *   6. wrong contact filtered out
 *   7. earliest/latest tracked across multiple facts
 *   8. render: empty summary → 0 bytes; populated → "WHAT WORKS: …"
 */

#include "human/memory/causal_attribution.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "human/memory/trust.h"
#include "test_framework.h"

#include <string.h>

static void seed_reaction_fact(hu_personal_model_t *m, const char *contact, const char *predicate,
                               int64_t ts) {
    if (m->fact_count >= HU_PM_MAX_FACTS)
        return;
    hu_heuristic_fact_t *f = &m->facts[m->fact_count++];
    memset(f, 0, sizeof(*f));
    f->type = HU_KNOWLEDGE_PROPOSITIONAL;
    snprintf(f->subject, sizeof(f->subject), "%s", contact);
    snprintf(f->predicate, sizeof(f->predicate), "%s", predicate);
    snprintf(f->object, sizeof(f->object), "%s", "hiking");
    snprintf(f->source_hint, sizeof(f->source_hint), "reaction_ingest");
    f->confidence = 0.7f;
    f->last_seen_at = ts;
    snprintf(f->provenance.contact_handle, sizeof(f->provenance.contact_handle), "%s", contact);
    f->provenance.tier = HU_TRUST_THIRD_PARTY;
}

static void seed_non_reaction_fact(hu_personal_model_t *m, const char *contact) {
    if (m->fact_count >= HU_PM_MAX_FACTS)
        return;
    hu_heuristic_fact_t *f = &m->facts[m->fact_count++];
    memset(f, 0, sizeof(*f));
    snprintf(f->subject, sizeof(f->subject), "%s", contact);
    snprintf(f->predicate, sizeof(f->predicate), "%s", "loves");
    snprintf(f->object, sizeof(f->object), "%s", "espresso");
    snprintf(f->source_hint, sizeof(f->source_hint), "conversation");
    f->confidence = 0.8f;
    f->last_seen_at = 1000;
    snprintf(f->provenance.contact_handle, sizeof(f->provenance.contact_handle), "%s", contact);
}

static void test_empty_model_returns_zero(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    hu_causal_attribution_summary_t s;
    HU_ASSERT_EQ((int)hu_causal_attribution_summarize(&m, "alice", &s), 0);
    HU_ASSERT_EQ((int)s.total_reactions, 0);
}

static void test_non_reaction_facts_ignored(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_non_reaction_fact(&m, "alice");
    hu_causal_attribution_summary_t s;
    HU_ASSERT_EQ((int)hu_causal_attribution_summarize(&m, "alice", &s), 0);
}

static void test_positive_verb_counted(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_reaction_fact(&m, "alice", "loves", 1000);
    seed_reaction_fact(&m, "alice", "likes", 2000);
    hu_causal_attribution_summary_t s;
    hu_causal_attribution_summarize(&m, "alice", &s);
    HU_ASSERT_EQ((int)s.positive_count, 2);
    HU_ASSERT_EQ((int)s.negative_count, 0);
}

static void test_negative_verb_counted(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_reaction_fact(&m, "alice", "hates", 1000);
    seed_reaction_fact(&m, "alice", "dislikes", 2000);
    hu_causal_attribution_summary_t s;
    hu_causal_attribution_summarize(&m, "alice", &s);
    HU_ASSERT_EQ((int)s.negative_count, 2);
    HU_ASSERT_EQ((int)s.positive_count, 0);
}

static void test_unknown_predicate_neutral(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_reaction_fact(&m, "alice", "mentioned", 1000);
    hu_causal_attribution_summary_t s;
    hu_causal_attribution_summarize(&m, "alice", &s);
    HU_ASSERT_EQ((int)s.neutral_count, 1);
}

static void test_wrong_contact_filtered(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_reaction_fact(&m, "bob", "loves", 1000);
    hu_causal_attribution_summary_t s;
    HU_ASSERT_EQ((int)hu_causal_attribution_summarize(&m, "alice", &s), 0);
}

static void test_earliest_latest_tracked(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_reaction_fact(&m, "alice", "loves", 2000);
    seed_reaction_fact(&m, "alice", "likes", 1000);
    seed_reaction_fact(&m, "alice", "loves", 3000);
    hu_causal_attribution_summary_t s;
    hu_causal_attribution_summarize(&m, "alice", &s);
    HU_ASSERT_EQ((int)s.earliest_seen, 1000);
    HU_ASSERT_EQ((int)s.latest_seen, 3000);
}

static void test_render_empty_writes_nothing_populated_renders(void) {
    char buf[256] = {0};
    hu_causal_attribution_summary_t empty = {0};
    HU_ASSERT_EQ((int)hu_causal_attribution_render("alice", &empty, 1000, buf, sizeof(buf)), 0);

    hu_causal_attribution_summary_t s = {0};
    s.total_reactions = 6;
    s.positive_count = 5;
    s.negative_count = 1;
    s.latest_seen = 1000;
    /* `now` = 1000 + 7d → "(latest 7d ago)" branch. */
    size_t n = hu_causal_attribution_render("alice", &s, 1000 + 86400 * 7, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "WHAT WORKS:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "5 positive") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "1 negative") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "7d ago") != NULL);
}

void run_causal_attribution_tests(void) {
    HU_TEST_SUITE("causal_attribution");
    HU_RUN_TEST(test_empty_model_returns_zero);
    HU_RUN_TEST(test_non_reaction_facts_ignored);
    HU_RUN_TEST(test_positive_verb_counted);
    HU_RUN_TEST(test_negative_verb_counted);
    HU_RUN_TEST(test_unknown_predicate_neutral);
    HU_RUN_TEST(test_wrong_contact_filtered);
    HU_RUN_TEST(test_earliest_latest_tracked);
    HU_RUN_TEST(test_render_empty_writes_nothing_populated_renders);
}
