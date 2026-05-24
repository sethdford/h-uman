/* tests/test_identity_continuity.c
 *
 * Sprint B Story 8 — persistent identity continuity.
 * Contracts (8 tests):
 *   1. first_token_lower: extracts first alpha word, lowercased
 *   2. first_token_lower: skips leading punctuation/digits
 *   3. first_token_lower: empty/NULL → 0
 *   4. suggest: empty graph → 0
 *   5. suggest: handle already in graph → 0 (no spurious merge)
 *   6. suggest: handle matches canonical_name first token → renders
 *   7. suggest: handle has no name overlap → 0
 *   8. suggest: only first candidate surfaces (no flooding)
 */

#include "human/memory/fact_extract.h"
#include "human/memory/identity_continuity.h"
#include "human/memory/identity_resolver.h"
#include "human/memory/personal_model.h"
#include "human/memory/trust.h"
#include "test_framework.h"

#include <string.h>

static void seed_fact(hu_personal_model_t *m, const char *contact) {
    if (m->fact_count >= HU_PM_MAX_FACTS)
        return;
    hu_heuristic_fact_t *f = &m->facts[m->fact_count++];
    memset(f, 0, sizeof(*f));
    snprintf(f->subject, sizeof(f->subject), "%s", "they");
    snprintf(f->predicate, sizeof(f->predicate), "%s", "are");
    snprintf(f->object, sizeof(f->object), "%s", "ok");
    f->confidence = 0.7f;
    f->last_seen_at = 1000;
    snprintf(f->provenance.contact_handle, sizeof(f->provenance.contact_handle), "%s", contact);
}

static void make_graph_with_alice(hu_identity_graph_t *g) {
    const char *handles[] = {"+15551234567", "alice@gmail.com"};
    const char *channels[] = {"imessage", "email"};
    const char *names[] = {"Alice", "Alice"};
    hu_identity_resolve(handles, channels, names, 2, g);
}

/* ── first_token_lower tests ────────────────────────────────────────── */

static void test_first_token_extracts_alpha_word(void) {
    char buf[64] = {0};
    HU_ASSERT_TRUE(hu_identity_continuity_first_token_lower("Alice Smith", buf, sizeof(buf)) > 0);
    HU_ASSERT_STR_EQ(buf, "alice");
}

static void test_first_token_skips_leading_punct(void) {
    char buf[64] = {0};
    HU_ASSERT_TRUE(
        hu_identity_continuity_first_token_lower("+15551234567 Alice", buf, sizeof(buf)) > 0);
    HU_ASSERT_STR_EQ(buf, "alice");
}

static void test_first_token_empty_or_null_returns_zero(void) {
    char buf[64] = {0};
    HU_ASSERT_EQ((int)hu_identity_continuity_first_token_lower(NULL, buf, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_identity_continuity_first_token_lower("", buf, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_identity_continuity_first_token_lower("1234", buf, sizeof(buf)), 0);
}

/* ── suggest tests ──────────────────────────────────────────────────── */

static void test_suggest_empty_graph_writes_nothing(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    seed_fact(&m, "alice@work.example");
    hu_identity_graph_t g;
    memset(&g, 0, sizeof(g));
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_identity_continuity_suggest(&m, &g, buf, sizeof(buf)), 0);
}

static void test_suggest_handle_already_in_graph_no_merge(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Handle is "+15551234567" — already an alias of Alice in the graph. */
    seed_fact(&m, "+15551234567");
    hu_identity_graph_t g;
    make_graph_with_alice(&g);
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_identity_continuity_suggest(&m, &g, buf, sizeof(buf)), 0);
}

static void test_suggest_handle_name_token_match_renders(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* New handle starts with "alice" — token match to canonical_name. */
    seed_fact(&m, "alice@new-work-account.com");
    hu_identity_graph_t g;
    make_graph_with_alice(&g);
    char buf[256] = {0};
    size_t n = hu_identity_continuity_suggest(&m, &g, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "IDENTITY:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Alice") != NULL);
}

static void test_suggest_handle_no_overlap_writes_nothing(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* New handle is "+19998887777" — no name overlap. */
    seed_fact(&m, "+19998887777");
    hu_identity_graph_t g;
    make_graph_with_alice(&g);
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_identity_continuity_suggest(&m, &g, buf, sizeof(buf)), 0);
}

static void test_suggest_emits_only_first_candidate(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Two NEW handles both starting with "alice" — only ONE line
     * surfaces (no flooding). */
    seed_fact(&m, "alice@workspace.example");
    seed_fact(&m, "alice@another.example");
    hu_identity_graph_t g;
    make_graph_with_alice(&g);
    char buf[256] = {0};
    size_t n = hu_identity_continuity_suggest(&m, &g, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    /* The string must contain only ONE "IDENTITY:" prefix. */
    const char *first = strstr(buf, "IDENTITY:");
    HU_ASSERT_NOT_NULL(first);
    const char *second = strstr(first + 1, "IDENTITY:");
    HU_ASSERT_TRUE(second == NULL);
}

void run_identity_continuity_tests(void) {
    HU_TEST_SUITE("identity_continuity");
    HU_RUN_TEST(test_first_token_extracts_alpha_word);
    HU_RUN_TEST(test_first_token_skips_leading_punct);
    HU_RUN_TEST(test_first_token_empty_or_null_returns_zero);
    HU_RUN_TEST(test_suggest_empty_graph_writes_nothing);
    HU_RUN_TEST(test_suggest_handle_already_in_graph_no_merge);
    HU_RUN_TEST(test_suggest_handle_name_token_match_renders);
    HU_RUN_TEST(test_suggest_handle_no_overlap_writes_nothing);
    HU_RUN_TEST(test_suggest_emits_only_first_candidate);
}
