/* tests/test_outbound_sanitize.c
 *
 * Unit tests for the legacy `hu_outbound_sanitize` compatibility shim
 * (src/agent/outbound_sanitize.c). The shim adapts the old in-place
 * sanitize API onto the Sprint 59 6-stage outbound pipeline. These tests
 * exercise the API contract directly — no network, no spawning,
 * deterministic.
 *
 * Contract under test:
 *   - NULL content / NULL length pointer -> false, reason "null_input"
 *   - zero-length content                -> false, reason "empty_input"
 *   - clean human-voice content          -> true (SEND), content unchanged
 *   - length is updated in-place when the pipeline shrinks the body
 */

#include "test_framework.h"

#include "human/agent/outbound_sanitize.h"

#include <string.h>

/* NULL inputs are rejected with the documented reason and never crash. */
static void test_outbound_sanitize_null_content_returns_false(void) {
    size_t len = 5;
    const char *reason = NULL;
    bool ok = hu_outbound_sanitize(NULL, &len, &reason);
    HU_ASSERT_FALSE(ok);
    HU_ASSERT_NOT_NULL(reason);
    HU_ASSERT_STR_EQ(reason, "null_input");
}

static void test_outbound_sanitize_null_len_returns_false(void) {
    char buf[] = "hello";
    const char *reason = NULL;
    bool ok = hu_outbound_sanitize(buf, NULL, &reason);
    HU_ASSERT_FALSE(ok);
    HU_ASSERT_NOT_NULL(reason);
    HU_ASSERT_STR_EQ(reason, "null_input");
}

/* Empty content (len == 0) is rejected with "empty_input". */
static void test_outbound_sanitize_empty_returns_false(void) {
    char buf[] = "";
    size_t len = 0;
    const char *reason = NULL;
    bool ok = hu_outbound_sanitize(buf, &len, &reason);
    HU_ASSERT_FALSE(ok);
    HU_ASSERT_NOT_NULL(reason);
    HU_ASSERT_STR_EQ(reason, "empty_input");
}

/* A clean, in-voice message passes (SEND) and the buffer + length are
 * preserved. The pipeline can only shrink, so a no-violation body must
 * come back unchanged with the same length. */
static void test_outbound_sanitize_clean_message_passes_unchanged(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "yeah sounds good, see you then");
    size_t orig_len = strlen(buf);
    size_t len = orig_len;
    const char *reason = NULL;

    bool ok = hu_outbound_sanitize(buf, &len, &reason);

    HU_ASSERT_TRUE(ok);
    /* SEND must not grow the body; clean input must not be truncated. */
    HU_ASSERT(len <= orig_len);
    HU_ASSERT_EQ((int)len, (int)orig_len);
    HU_ASSERT_STR_EQ(buf, "yeah sounds good, see you then");
}

/* The returned length never exceeds the original buffer length — the
 * shim rejects rewrite-expansion conservatively rather than overflowing
 * the caller's buffer. */
static void test_outbound_sanitize_never_grows_length(void) {
    char buf[128];
    snprintf(buf, sizeof(buf), "ok");
    size_t orig_len = strlen(buf);
    size_t len = orig_len;
    const char *reason = NULL;

    bool ok = hu_outbound_sanitize(buf, &len, &reason);
    (void)ok; /* verdict may be SEND or REJECT depending on stage config */
    HU_ASSERT(len <= orig_len);
}

void run_outbound_sanitize_tests(void) {
    HU_TEST_SUITE("outbound_sanitize");
    HU_RUN_TEST(test_outbound_sanitize_null_content_returns_false);
    HU_RUN_TEST(test_outbound_sanitize_null_len_returns_false);
    HU_RUN_TEST(test_outbound_sanitize_empty_returns_false);
    HU_RUN_TEST(test_outbound_sanitize_clean_message_passes_unchanged);
    HU_RUN_TEST(test_outbound_sanitize_never_grows_length);
}
