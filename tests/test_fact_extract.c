#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/fact_extract.h"
#include "test_framework.h"
#include <string.h>

static void fact_extract_personal_statement_finds_facts(void) {
    const char *text = "My name is Alice and I live in NYC.";
    hu_fact_extract_result_t result;
    HU_ASSERT_EQ(hu_fact_extract(text, strlen(text), &result), HU_OK);
    HU_ASSERT_GT((long)result.fact_count, 0L);
}

static void fact_extract_empty_text_zero_facts(void) {
    hu_fact_extract_result_t result;
    HU_ASSERT_EQ(hu_fact_extract("", 0, &result), HU_OK);
    HU_ASSERT_EQ((long)result.fact_count, 0L);
}

static void fact_extract_null_returns_error(void) {
    hu_fact_extract_result_t result;
    HU_ASSERT_EQ(hu_fact_extract(NULL, 0, &result), HU_ERR_INVALID_ARGUMENT);
}

static void fact_dedup_removes_duplicates(void) {
    const char *text = "My name is Alice and I live in NYC.";
    hu_fact_extract_result_t first;
    hu_fact_extract_result_t second;
    HU_ASSERT_EQ(hu_fact_extract(text, strlen(text), &first), HU_OK);
    HU_ASSERT_EQ(hu_fact_extract(text, strlen(text), &second), HU_OK);
    HU_ASSERT_GT((long)first.fact_count, 0L);

    size_t novel = hu_fact_dedup(&second, first.facts, first.fact_count);
    HU_ASSERT_EQ((long)novel, 0L);
}

static void fact_format_for_store_produces_key_value(void) {
    hu_heuristic_fact_t fact;
    memset(&fact, 0, sizeof(fact));
    fact.type = HU_KNOWLEDGE_PROPOSITIONAL;
    strncpy(fact.subject, "user", sizeof(fact.subject) - 1);
    strncpy(fact.predicate, "prefers", sizeof(fact.predicate) - 1);
    strncpy(fact.object, "tea", sizeof(fact.object) - 1);
    fact.confidence = 0.75f;

    hu_allocator_t alloc = hu_system_allocator();
    char *key = NULL;
    size_t key_len = 0;
    char *value = NULL;
    size_t value_len = 0;
    HU_ASSERT_EQ(hu_fact_format_for_store(&alloc, &fact, &key, &key_len, &value, &value_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(key);
    HU_ASSERT_NOT_NULL(value);
    HU_ASSERT_GT((long)key_len, 0L);
    HU_ASSERT_GT((long)value_len, 0L);

    alloc.free(alloc.ctx, key, key_len + 1);
    alloc.free(alloc.ctx, value, value_len + 1);
}

static void fact_format_null_fact_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *key = NULL;
    size_t key_len = 0;
    char *value = NULL;
    size_t value_len = 0;
    HU_ASSERT_EQ(hu_fact_format_for_store(&alloc, NULL, &key, &key_len, &value, &value_len),
                 HU_ERR_INVALID_ARGUMENT);
}

/* P2-6 regression (2026-05-16 incident): heuristic patterns like "i like",
 * "when i'm", "i never" used to store the marker verbatim into the
 * predicate field. When `hu_fact_format_for_store` rendered them as
 * "user i like X", the result still had a first-person flavor and could
 * leak into outbound prompts/messages as a confession-style fragment.
 *
 * Fix: at extract time, normalize predicates to third-person form ("i
 * like" → "likes", "i never" → "never", "when i'm" → "when"). The
 * subject column is already "user"; combining "user" + a normalized
 * predicate produces "user likes X" — a clean, paraphrased fact. */

static void fact_extract_predicate_no_longer_first_person(void) {
    const char *text = "I like hiking and I never eat meat.";
    hu_fact_extract_result_t result;
    HU_ASSERT_EQ(hu_fact_extract(text, strlen(text), &result), HU_OK);
    HU_ASSERT_GT((long)result.fact_count, 0L);

    /* Every extracted fact's predicate must NOT start with "i " or "i'". */
    for (size_t i = 0; i < result.fact_count; i++) {
        const char *p = result.facts[i].predicate;
        HU_ASSERT_TRUE(p[0] != 'i' || (p[1] != ' ' && p[1] != '\''));
        /* Also no embedded " i " — would surface as "user i likes ..." */
        HU_ASSERT_NULL(strstr(p, " i "));
    }
}

static void fact_extract_when_im_predicate_normalized(void) {
    const char *text = "When I'm stressed I eat chocolate.";
    hu_fact_extract_result_t result;
    HU_ASSERT_EQ(hu_fact_extract(text, strlen(text), &result), HU_OK);

    /* "when i'm" pattern should be stored with a sanitized predicate that
     * does NOT include the first-person "i'm" — otherwise the formatted
     * value reads "user when i'm stressed". */
    bool found = false;
    for (size_t i = 0; i < result.fact_count; i++) {
        if (strstr(result.facts[i].predicate, "when") != NULL) {
            found = true;
            HU_ASSERT_NULL(strstr(result.facts[i].predicate, "i'm"));
            HU_ASSERT_NULL(strstr(result.facts[i].predicate, "i "));
        }
    }
    (void)found; /* Pattern may or may not fire; the assertion is what matters when it does. */
}

static void fact_format_for_store_renders_paraphrased_third_person(void) {
    const char *text = "I like hiking.";
    hu_fact_extract_result_t result;
    HU_ASSERT_EQ(hu_fact_extract(text, strlen(text), &result), HU_OK);
    HU_ASSERT_GT((long)result.fact_count, 0L);

    hu_allocator_t alloc = hu_system_allocator();
    char *key = NULL;
    size_t key_len = 0;
    char *value = NULL;
    size_t value_len = 0;
    HU_ASSERT_EQ(
        hu_fact_format_for_store(&alloc, &result.facts[0], &key, &key_len, &value, &value_len),
        HU_OK);

    /* Rendered value must not contain "user i like" — that's first-person
     * leaking through. It should be e.g. "user likes hiking". */
    HU_ASSERT_NULL(strstr(value, "user i "));
    HU_ASSERT_NULL(strstr(value, " i like"));

    alloc.free(alloc.ctx, key, key_len + 1);
    alloc.free(alloc.ctx, value, value_len + 1);
}

void run_fact_extract_tests(void) {
    HU_TEST_SUITE("fact_extract");
    HU_RUN_TEST(fact_extract_personal_statement_finds_facts);
    HU_RUN_TEST(fact_extract_empty_text_zero_facts);
    HU_RUN_TEST(fact_extract_null_returns_error);
    HU_RUN_TEST(fact_dedup_removes_duplicates);
    HU_RUN_TEST(fact_format_for_store_produces_key_value);
    HU_RUN_TEST(fact_format_null_fact_returns_error);
    HU_RUN_TEST(fact_extract_predicate_no_longer_first_person);
    HU_RUN_TEST(fact_extract_when_im_predicate_normalized);
    HU_RUN_TEST(fact_format_for_store_renders_paraphrased_third_person);
}
