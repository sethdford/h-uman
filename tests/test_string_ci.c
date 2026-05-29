/* Tests for the canonical case-insensitive substring helpers in
 * include/human/core/string.h. These are the consolidation target for the
 * ~20 private str_contains_ci / ci_contains / *_word_ci statics that used to
 * live scattered across src/. The contract pinned here is what every migrated
 * call site relies on. */
#include "human/core/string.h"
#include "test_framework.h"

/* ---- hu_str_contains_ci (bounded, both sides length-explicit) ---- */

static void test_str_contains_ci_basic_match(void) {
    HU_ASSERT_TRUE(hu_str_contains_ci("Hello World", 11, "world", 5));
    HU_ASSERT_TRUE(hu_str_contains_ci("HELLO", 5, "ell", 3));
    HU_ASSERT_TRUE(hu_str_contains_ci("abcdef", 6, "abc", 3));
    HU_ASSERT_TRUE(hu_str_contains_ci("abcdef", 6, "def", 3));
}

static void test_str_contains_ci_no_match(void) {
    HU_ASSERT_FALSE(hu_str_contains_ci("Hello World", 11, "xyz", 3));
}

static void test_str_contains_ci_null_safe(void) {
    HU_ASSERT_FALSE(hu_str_contains_ci(NULL, 0, "x", 1));
    HU_ASSERT_FALSE(hu_str_contains_ci("x", 1, NULL, 1));
    HU_ASSERT_FALSE(hu_str_contains_ci(NULL, 5, NULL, 2));
}

static void test_str_contains_ci_empty_needle_is_false(void) {
    /* Canonical semantics: empty needle returns false (the majority of the
     * private copies behaved this way; no live caller passes an empty
     * needle). */
    HU_ASSERT_FALSE(hu_str_contains_ci("anything", 8, "", 0));
}

static void test_str_contains_ci_needle_longer_than_haystack(void) {
    HU_ASSERT_FALSE(hu_str_contains_ci("ab", 2, "abc", 3));
}

static void test_str_contains_ci_respects_bounds_not_nul(void) {
    /* hlen/nlen are authoritative — bytes past the bound are invisible even
     * if the underlying C string continues. */
    HU_ASSERT_FALSE(hu_str_contains_ci("abcdef", 3, "def", 3));
    HU_ASSERT_TRUE(hu_str_contains_ci("abcdef", 3, "abc", 3));
    /* needle bound shorter than its NUL string */
    HU_ASSERT_TRUE(hu_str_contains_ci("zzabczz", 7, "abcXX", 3));
}

static void test_str_contains_ci_match_at_end(void) {
    HU_ASSERT_TRUE(hu_str_contains_ci("foobar", 6, "BAR", 3));
}

/* ---- hu_str_contains_ci_cstr (NUL-terminated needle convenience) ---- */

static void test_str_contains_ci_cstr_matches(void) {
    HU_ASSERT_TRUE(hu_str_contains_ci_cstr("Hello World", 11, "WORLD"));
    HU_ASSERT_FALSE(hu_str_contains_ci_cstr("Hello World", 11, "nope"));
}

static void test_str_contains_ci_cstr_null_safe(void) {
    HU_ASSERT_FALSE(hu_str_contains_ci_cstr("x", 1, NULL));
    HU_ASSERT_FALSE(hu_str_contains_ci_cstr(NULL, 0, "x"));
}

static void test_str_contains_ci_cstr_respects_haystack_bound(void) {
    HU_ASSERT_FALSE(hu_str_contains_ci_cstr("abcdef", 3, "def"));
    HU_ASSERT_TRUE(hu_str_contains_ci_cstr("abcdef", 3, "abc"));
}

/* ---- hu_str_contains_word_ci (word-boundary, NUL/NUL) ---- */
/* See ~/.claude/rules/substring-classifier-pitfalls.md for the rationale. */

static void test_word_ci_matches_whole_word(void) {
    HU_ASSERT_TRUE(hu_str_contains_word_ci("warm friend", "warm"));
    HU_ASSERT_TRUE(hu_str_contains_word_ci("warm friend", "friend"));
    HU_ASSERT_TRUE(hu_str_contains_word_ci("formal", "formal"));
}

static void test_word_ci_case_insensitive(void) {
    HU_ASSERT_TRUE(hu_str_contains_word_ci("Be WARM and kind", "warm"));
}

static void test_word_ci_rejects_substring_overlap(void) {
    /* The whole point: opposite-intent inputs must NOT match. */
    HU_ASSERT_FALSE(hu_str_contains_word_ci("informal", "formal"));
    HU_ASSERT_FALSE(hu_str_contains_word_ci("lukewarm", "warm"));
    HU_ASSERT_FALSE(hu_str_contains_word_ci("unfriendly", "friend"));
    HU_ASSERT_FALSE(hu_str_contains_word_ci("unprofessional", "professional"));
}

static void test_word_ci_boundary_chars(void) {
    /* Anything not [A-Za-z0-9] bounds a word: space, hyphen, underscore,
     * comma, dot. */
    HU_ASSERT_TRUE(hu_str_contains_word_ci("close-friend", "close"));
    HU_ASSERT_TRUE(hu_str_contains_word_ci("close_friend", "friend"));
    HU_ASSERT_TRUE(hu_str_contains_word_ci("warm, kind", "warm"));
    HU_ASSERT_TRUE(hu_str_contains_word_ci("a.warm.day", "warm"));
}

static void test_word_ci_null_safe(void) {
    HU_ASSERT_FALSE(hu_str_contains_word_ci(NULL, "x"));
    HU_ASSERT_FALSE(hu_str_contains_word_ci("x", NULL));
    HU_ASSERT_FALSE(hu_str_contains_word_ci("x", ""));
}

static void test_word_ci_needle_longer_than_haystack(void) {
    HU_ASSERT_FALSE(hu_str_contains_word_ci("hi", "hello"));
}

/* ---- hu_str_contains_word_ci_n (length-bounded word-boundary) ---- */

static void test_word_ci_n_matches_within_bound(void) {
    HU_ASSERT_TRUE(hu_str_contains_word_ci_n("warm friend", 11, "warm"));
    HU_ASSERT_TRUE(hu_str_contains_word_ci_n("warm friend", 11, "friend"));
    HU_ASSERT_TRUE(hu_str_contains_word_ci_n("Be WARM here", 12, "warm"));
}

static void test_word_ci_n_rejects_substring_overlap(void) {
    /* Same anti-overlap contract as the NUL variant, but length-bounded. */
    HU_ASSERT_FALSE(hu_str_contains_word_ci_n("informal", 8, "formal"));
    HU_ASSERT_FALSE(hu_str_contains_word_ci_n("lukewarm", 8, "warm"));
    HU_ASSERT_FALSE(hu_str_contains_word_ci_n("unfriendly", 10, "friend"));
}

static void test_word_ci_n_bound_acts_as_right_boundary(void) {
    /* The byte at hlen is end-of-string for boundary purposes: "warmly"
     * read for only 4 bytes is the standalone word "warm". */
    HU_ASSERT_TRUE(hu_str_contains_word_ci_n("warmly", 4, "warm"));
    /* ...but reading the full 6 bytes, "warm" is no longer word-bounded on
     * the right ('l' is alnum), so it must NOT match. */
    HU_ASSERT_FALSE(hu_str_contains_word_ci_n("warmly", 6, "warm"));
}

static void test_word_ci_n_ignores_bytes_past_bound(void) {
    /* Bytes past hlen are invisible even though the C string continues. */
    HU_ASSERT_FALSE(hu_str_contains_word_ci_n("warm friend", 4, "friend"));
    HU_ASSERT_TRUE(hu_str_contains_word_ci_n("warm friend", 4, "warm"));
}

static void test_word_ci_n_null_safe(void) {
    HU_ASSERT_FALSE(hu_str_contains_word_ci_n(NULL, 4, "warm"));
    HU_ASSERT_FALSE(hu_str_contains_word_ci_n("warm", 4, NULL));
    HU_ASSERT_FALSE(hu_str_contains_word_ci_n("warm", 4, ""));
    HU_ASSERT_FALSE(hu_str_contains_word_ci_n("warm", 0, "warm"));
}

void run_string_ci_tests(void) {
    HU_TEST_SUITE("string_ci");
    HU_RUN_TEST(test_str_contains_ci_basic_match);
    HU_RUN_TEST(test_str_contains_ci_no_match);
    HU_RUN_TEST(test_str_contains_ci_null_safe);
    HU_RUN_TEST(test_str_contains_ci_empty_needle_is_false);
    HU_RUN_TEST(test_str_contains_ci_needle_longer_than_haystack);
    HU_RUN_TEST(test_str_contains_ci_respects_bounds_not_nul);
    HU_RUN_TEST(test_str_contains_ci_match_at_end);
    HU_RUN_TEST(test_str_contains_ci_cstr_matches);
    HU_RUN_TEST(test_str_contains_ci_cstr_null_safe);
    HU_RUN_TEST(test_str_contains_ci_cstr_respects_haystack_bound);
    HU_RUN_TEST(test_word_ci_matches_whole_word);
    HU_RUN_TEST(test_word_ci_case_insensitive);
    HU_RUN_TEST(test_word_ci_rejects_substring_overlap);
    HU_RUN_TEST(test_word_ci_boundary_chars);
    HU_RUN_TEST(test_word_ci_null_safe);
    HU_RUN_TEST(test_word_ci_needle_longer_than_haystack);
    HU_RUN_TEST(test_word_ci_n_matches_within_bound);
    HU_RUN_TEST(test_word_ci_n_rejects_substring_overlap);
    HU_RUN_TEST(test_word_ci_n_bound_acts_as_right_boundary);
    HU_RUN_TEST(test_word_ci_n_ignores_bytes_past_bound);
    HU_RUN_TEST(test_word_ci_n_null_safe);
}
