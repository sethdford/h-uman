// @covers-none — exercises hu_imessage_private_* from
// src/channels/imessage_private/protocol.c. The filename→module heuristic
// resolves "imessage_private_protocol" → "imessage" (wrong module, since the
// real source is nested under imessage_private/), so opt out; the production
// symbols are called directly throughout.
#include "human/channels/imessage_private/protocol.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* ── port formula: clamp(45670 + uid - 501, 45670, 65535) ─────────────── */

static void port_anchor_uid_is_base(void) {
    HU_ASSERT_EQ(hu_imessage_private_port_for_uid(501), 45670);
}

static void port_increments_with_uid(void) {
    HU_ASSERT_EQ(hu_imessage_private_port_for_uid(502), 45671);
    HU_ASSERT_EQ(hu_imessage_private_port_for_uid(1000), 45670 + (1000 - 501));
}

static void port_below_anchor_clamps_to_base(void) {
    /* uid 0 would compute 45670-501 < base → clamp up to base. */
    HU_ASSERT_EQ(hu_imessage_private_port_for_uid(0), 45670);
    HU_ASSERT_EQ(hu_imessage_private_port_for_uid(500), 45670);
}

static void port_huge_uid_clamps_to_max(void) {
    HU_ASSERT_EQ(hu_imessage_private_port_for_uid(999999), 65535);
}

/* ── mode parsing: case-insensitive, unknown → OFF ────────────────────── */

static void mode_from_string_recognizes_all(void) {
    HU_ASSERT_EQ((int)hu_imessage_private_mode_from_string("off"), (int)HU_IMESSAGE_PRIVATE_MODE_OFF);
    HU_ASSERT_EQ((int)hu_imessage_private_mode_from_string("shadow"),
                 (int)HU_IMESSAGE_PRIVATE_MODE_SHADOW);
    HU_ASSERT_EQ((int)hu_imessage_private_mode_from_string("live"),
                 (int)HU_IMESSAGE_PRIVATE_MODE_LIVE);
}

static void mode_from_string_is_case_insensitive(void) {
    HU_ASSERT_EQ((int)hu_imessage_private_mode_from_string("LIVE"),
                 (int)HU_IMESSAGE_PRIVATE_MODE_LIVE);
    HU_ASSERT_EQ((int)hu_imessage_private_mode_from_string("Shadow"),
                 (int)HU_IMESSAGE_PRIVATE_MODE_SHADOW);
}

static void mode_from_string_unknown_and_null_are_off(void) {
    HU_ASSERT_EQ((int)hu_imessage_private_mode_from_string(NULL),
                 (int)HU_IMESSAGE_PRIVATE_MODE_OFF);
    HU_ASSERT_EQ((int)hu_imessage_private_mode_from_string(""), (int)HU_IMESSAGE_PRIVATE_MODE_OFF);
    HU_ASSERT_EQ((int)hu_imessage_private_mode_from_string("enabled"),
                 (int)HU_IMESSAGE_PRIVATE_MODE_OFF);
}

static void mode_name_round_trips(void) {
    HU_ASSERT_STR_EQ(hu_imessage_private_mode_name(HU_IMESSAGE_PRIVATE_MODE_OFF), "off");
    HU_ASSERT_STR_EQ(hu_imessage_private_mode_name(HU_IMESSAGE_PRIVATE_MODE_SHADOW), "shadow");
    HU_ASSERT_STR_EQ(hu_imessage_private_mode_name(HU_IMESSAGE_PRIVATE_MODE_LIVE), "live");
}

/* ── line framing ─────────────────────────────────────────────────────── */

static void line_buf_single_line(void) {
    hu_imsg_line_buf_t b;
    hu_imsg_line_buf_init(&b);
    HU_ASSERT_TRUE(hu_imsg_line_buf_append(&b, "hello\n", 6));
    size_t n = 0;
    char *line = hu_imsg_line_buf_next(&b, &n);
    HU_ASSERT_NOT_NULL(line);
    HU_ASSERT_STR_EQ(line, "hello");
    HU_ASSERT_EQ((int)n, 5);
    free(line);
    HU_ASSERT_TRUE(hu_imsg_line_buf_next(&b, NULL) == NULL);
    hu_imsg_line_buf_free(&b);
}

static void line_buf_strips_trailing_cr(void) {
    hu_imsg_line_buf_t b;
    hu_imsg_line_buf_init(&b);
    HU_ASSERT_TRUE(hu_imsg_line_buf_append(&b, "{\"a\":1}\r\n", 9));
    char *line = hu_imsg_line_buf_next(&b, NULL);
    HU_ASSERT_NOT_NULL(line);
    HU_ASSERT_STR_EQ(line, "{\"a\":1}");
    free(line);
    hu_imsg_line_buf_free(&b);
}

static void line_buf_multiple_lines_one_append(void) {
    hu_imsg_line_buf_t b;
    hu_imsg_line_buf_init(&b);
    HU_ASSERT_TRUE(hu_imsg_line_buf_append(&b, "a\r\nbb\r\nccc\r\n", 12));
    char *l1 = hu_imsg_line_buf_next(&b, NULL);
    char *l2 = hu_imsg_line_buf_next(&b, NULL);
    char *l3 = hu_imsg_line_buf_next(&b, NULL);
    HU_ASSERT_STR_EQ(l1, "a");
    HU_ASSERT_STR_EQ(l2, "bb");
    HU_ASSERT_STR_EQ(l3, "ccc");
    HU_ASSERT_TRUE(hu_imsg_line_buf_next(&b, NULL) == NULL);
    free(l1);
    free(l2);
    free(l3);
    hu_imsg_line_buf_free(&b);
}

static void line_buf_partial_then_completed(void) {
    hu_imsg_line_buf_t b;
    hu_imsg_line_buf_init(&b);
    HU_ASSERT_TRUE(hu_imsg_line_buf_append(&b, "par", 3));
    HU_ASSERT_TRUE(hu_imsg_line_buf_next(&b, NULL) == NULL); /* no newline yet */
    HU_ASSERT_TRUE(hu_imsg_line_buf_append(&b, "tial\n", 5));
    char *line = hu_imsg_line_buf_next(&b, NULL);
    HU_ASSERT_NOT_NULL(line);
    HU_ASSERT_STR_EQ(line, "partial");
    free(line);
    hu_imsg_line_buf_free(&b);
}

static void line_buf_skips_empty_lines(void) {
    hu_imsg_line_buf_t b;
    hu_imsg_line_buf_init(&b);
    HU_ASSERT_TRUE(hu_imsg_line_buf_append(&b, "\r\n\r\nreal\r\n", 10));
    char *line = hu_imsg_line_buf_next(&b, NULL);
    HU_ASSERT_NOT_NULL(line);
    HU_ASSERT_STR_EQ(line, "real");
    free(line);
    HU_ASSERT_TRUE(hu_imsg_line_buf_next(&b, NULL) == NULL);
    hu_imsg_line_buf_free(&b);
}

static void line_buf_free_with_pending_is_clean(void) {
    /* Pending un-popped bytes must be freed by _free (ASan would flag a leak). */
    hu_imsg_line_buf_t b;
    hu_imsg_line_buf_init(&b);
    HU_ASSERT_TRUE(hu_imsg_line_buf_append(&b, "no newline here", 15));
    hu_imsg_line_buf_free(&b);
    /* free is idempotent / safe to call again on the zeroed struct. */
    hu_imsg_line_buf_free(&b);
}

void run_imessage_private_protocol_tests(void) {
    HU_TEST_SUITE("imessage_private_protocol");
    HU_RUN_TEST(port_anchor_uid_is_base);
    HU_RUN_TEST(port_increments_with_uid);
    HU_RUN_TEST(port_below_anchor_clamps_to_base);
    HU_RUN_TEST(port_huge_uid_clamps_to_max);
    HU_RUN_TEST(mode_from_string_recognizes_all);
    HU_RUN_TEST(mode_from_string_is_case_insensitive);
    HU_RUN_TEST(mode_from_string_unknown_and_null_are_off);
    HU_RUN_TEST(mode_name_round_trips);
    HU_RUN_TEST(line_buf_single_line);
    HU_RUN_TEST(line_buf_strips_trailing_cr);
    HU_RUN_TEST(line_buf_multiple_lines_one_append);
    HU_RUN_TEST(line_buf_partial_then_completed);
    HU_RUN_TEST(line_buf_skips_empty_lines);
    HU_RUN_TEST(line_buf_free_with_pending_is_clean);
}
