/* tests/test_mlx_stream_utf8.c
 *
 * Sprint 55 US-M3-B4 (Phase 2) — UTF-8 chunk-emission contract tests.
 *
 * These tests pin the safety contract of the helpers extracted from
 * src/providers/mlx.c. The Phase 1 ship had ZERO test coverage of
 * this logic because the subprocess driver that uses it is gated by
 * HU_MLX_SUBPROCESS_ACTIVE (off under HU_IS_TEST). The helpers are
 * load-bearing for correctness on any UTF-8 stream that lands mid-
 * codepoint at a read boundary — i.e., the common case for emoji,
 * accented characters, and CJK text.
 *
 * Test discipline:
 *   - No `// allow-silent-pass` opt-outs.
 *   - Every assertion exercises the real contract (chunked emission
 *     boundary, not a stub).
 *   - All inputs are real UTF-8 byte sequences; the tests fail loud
 *     if the helpers diverge from "never split a codepoint."
 */

#include "test_framework.h"

#include "human/providers/mlx_stream_utf8.h"

#include <string.h>

/* ── codepoint_len: lead-byte classification ──────────────────────── */

static void test_codepoint_len_ascii_is_one(void) {
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len('A'), 1);
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len('\0'), 1);
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len(0x7F), 1);
}

static void test_codepoint_len_2byte_lead_returns_2(void) {
    /* 110xxxxx — 2-byte lead. 0xC3 is the 'é' lead byte in UTF-8. */
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len(0xC3), 2);
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len(0xDF), 2);
}

static void test_codepoint_len_3byte_lead_returns_3(void) {
    /* 1110xxxx — 3-byte lead. 0xE2 leads CJK and many symbols. */
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len(0xE2), 3);
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len(0xEF), 3);
}

static void test_codepoint_len_4byte_lead_returns_4(void) {
    /* 11110xxx — 4-byte lead. 0xF0 leads emoji like U+1F600 ('😀'). */
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len(0xF0), 4);
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len(0xF4), 4);
}

static void test_codepoint_len_malformed_lead_returns_1_defensively(void) {
    /* A continuation byte (10xxxxxx) appearing as a "lead" is
     * malformed input. The helper returns 1 so the streaming driver
     * advances rather than stalls. */
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len(0x80), 1);
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len(0xBF), 1);
    /* 5-byte lead (0b11111xxx) is also out of spec — defensive 1. */
    HU_ASSERT_EQ((int)hu_mlx_utf8_codepoint_len(0xF8), 1);
}

/* ── safe_emit_len: chunk-boundary trimming ──────────────────────── */

static void test_safe_emit_len_empty_buffer_returns_zero(void) {
    HU_ASSERT_EQ((int)hu_mlx_utf8_safe_emit_len("", 0), 0);
}

static void test_safe_emit_len_null_buffer_returns_zero(void) {
    /* Defensive: NULL buf should not deref. */
    HU_ASSERT_EQ((int)hu_mlx_utf8_safe_emit_len(NULL, 10), 0);
}

static void test_safe_emit_len_pure_ascii_returns_full_length(void) {
    const char *s = "Hello, world!";
    size_t n = strlen(s);
    HU_ASSERT_EQ((int)hu_mlx_utf8_safe_emit_len(s, n), (int)n);
}

static void test_safe_emit_len_complete_2byte_tail_returns_full_length(void) {
    /* "café" = 'c','a','f',0xC3,0xA9. Codepoint U+00E9 (é) = 2 bytes,
     * fully present. Safe-emit len = full length. */
    const char buf[] = {'c', 'a', 'f', (char)0xC3, (char)0xA9};
    HU_ASSERT_EQ((int)hu_mlx_utf8_safe_emit_len(buf, sizeof(buf)), (int)sizeof(buf));
}

static void test_safe_emit_len_partial_2byte_tail_holds_back_one_byte(void) {
    /* "caf" + lone 0xC3 (lead of 2-byte sequence; continuation missing).
     * Safe-emit must HOLD the 0xC3 for the next read → returns 3. */
    const char buf[] = {'c', 'a', 'f', (char)0xC3};
    HU_ASSERT_EQ((int)hu_mlx_utf8_safe_emit_len(buf, sizeof(buf)), 3);
}

static void test_safe_emit_len_partial_3byte_tail_holds_back_two_bytes(void) {
    /* "abc" + 0xE2,0x82 (start of U+20AC '€' = E2 82 AC; AC missing).
     * Safe-emit must hold the 0xE2,0x82 pair → returns 3. */
    const char buf[] = {'a', 'b', 'c', (char)0xE2, (char)0x82};
    HU_ASSERT_EQ((int)hu_mlx_utf8_safe_emit_len(buf, sizeof(buf)), 3);
}

static void test_safe_emit_len_complete_3byte_tail_returns_full_length(void) {
    /* "abc" + U+20AC '€' (E2 82 AC) — fully present. */
    const char buf[] = {'a', 'b', 'c', (char)0xE2, (char)0x82, (char)0xAC};
    HU_ASSERT_EQ((int)hu_mlx_utf8_safe_emit_len(buf, sizeof(buf)), (int)sizeof(buf));
}

static void test_safe_emit_len_partial_4byte_emoji_tail_holds_back(void) {
    /* "hi " + 0xF0,0x9F,0x98 (start of U+1F600 '😀' = F0 9F 98 80;
     * final 0x80 missing). Safe-emit must hold the 3-byte partial
     * sequence → returns 3 (just "hi "). */
    const char buf[] = {'h', 'i', ' ', (char)0xF0, (char)0x9F, (char)0x98};
    HU_ASSERT_EQ((int)hu_mlx_utf8_safe_emit_len(buf, sizeof(buf)), 3);
}

static void test_safe_emit_len_complete_4byte_emoji_returns_full(void) {
    /* "hi " + '😀' (F0 9F 98 80) — fully present. */
    const char buf[] = {'h', 'i', ' ', (char)0xF0, (char)0x9F, (char)0x98, (char)0x80};
    HU_ASSERT_EQ((int)hu_mlx_utf8_safe_emit_len(buf, sizeof(buf)), (int)sizeof(buf));
}

static void test_safe_emit_len_single_continuation_byte_treated_as_safe(void) {
    /* A buffer of just 0x80 (continuation byte, no lead) is malformed.
     * Defensive contract: walk back the max (4) and return len so the
     * streaming driver doesn't stall on garbage input. */
    const char buf[] = {(char)0x80};
    HU_ASSERT_EQ((int)hu_mlx_utf8_safe_emit_len(buf, sizeof(buf)), 1);
}

static void test_safe_emit_len_ascii_after_complete_codepoint_returns_full(void) {
    /* Real-world stream pattern: "héllo" then more ASCII. Verify the
     * helper recognizes a completed codepoint mid-buffer and doesn't
     * accidentally trim because of multi-byte fear. */
    const char buf[] = {'h', (char)0xC3, (char)0xA9, 'l', 'l', 'o'};
    HU_ASSERT_EQ((int)hu_mlx_utf8_safe_emit_len(buf, sizeof(buf)), (int)sizeof(buf));
}

/* ── runner ───────────────────────────────────────────────────────── */

void run_mlx_stream_utf8_tests(void) {
    HU_TEST_SUITE("mlx_stream_utf8");

    HU_RUN_TEST(test_codepoint_len_ascii_is_one);
    HU_RUN_TEST(test_codepoint_len_2byte_lead_returns_2);
    HU_RUN_TEST(test_codepoint_len_3byte_lead_returns_3);
    HU_RUN_TEST(test_codepoint_len_4byte_lead_returns_4);
    HU_RUN_TEST(test_codepoint_len_malformed_lead_returns_1_defensively);

    HU_RUN_TEST(test_safe_emit_len_empty_buffer_returns_zero);
    HU_RUN_TEST(test_safe_emit_len_null_buffer_returns_zero);
    HU_RUN_TEST(test_safe_emit_len_pure_ascii_returns_full_length);
    HU_RUN_TEST(test_safe_emit_len_complete_2byte_tail_returns_full_length);
    HU_RUN_TEST(test_safe_emit_len_partial_2byte_tail_holds_back_one_byte);
    HU_RUN_TEST(test_safe_emit_len_partial_3byte_tail_holds_back_two_bytes);
    HU_RUN_TEST(test_safe_emit_len_complete_3byte_tail_returns_full_length);
    HU_RUN_TEST(test_safe_emit_len_partial_4byte_emoji_tail_holds_back);
    HU_RUN_TEST(test_safe_emit_len_complete_4byte_emoji_returns_full);
    HU_RUN_TEST(test_safe_emit_len_single_continuation_byte_treated_as_safe);
    HU_RUN_TEST(test_safe_emit_len_ascii_after_complete_codepoint_returns_full);
}
