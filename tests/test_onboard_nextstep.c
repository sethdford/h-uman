/* US-9.2 — Onboard post-success nextstep formatter unit tests.
 *
 * Pins the truth table for `hu_onboard_nextstep_format` (see
 * include/human/onboard.h docblock and sprints/sprint-9/designs/US-9.2.md).
 *
 * Adversarial tests assert the OLD generic messages are ABSENT
 * (strstr == NULL), per .claude/rules/tests-that-pin-bugs.md — a test
 * named "emits new message" that only checks `strstr(buf, "human")` would
 * pass against the buggy old text too.
 */

#include "human/core/error.h"
#include "human/onboard.h"
#include "test_framework.h"
#include <stddef.h>
#include <string.h>

#define TEST_PATH "/home/u/.human/config.json"

/* ------------------------------------------------------------------ */
/* Happy paths — one per row of the truth table.                       */
/* ------------------------------------------------------------------ */

static void test_already_exists_apple_shows_imessage_hint(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = TEST_PATH,
        .provider = "apple",
        .platform_is_apple = true,
        .already_exists = true,
        .parsed_ok = false, /* n/a when already_exists */
    };
    char buf[1024];
    hu_error_t err = hu_onboard_nextstep_format(&ctx, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(
        buf, "Config already exists at " TEST_PATH ".\n"
             "Run 'human doctor' to check status, or 'human doctor imessage' to pair iMessage.\n");
}

static void test_already_exists_nonapple_omits_imessage_hint(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = TEST_PATH,
        .provider = "gemini",
        .platform_is_apple = false,
        .already_exists = true,
        .parsed_ok = false,
    };
    char buf[1024];
    hu_error_t err = hu_onboard_nextstep_format(&ctx, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(buf, "Config already exists at " TEST_PATH ".\n"
                          "Run 'human doctor' to check status.\n");
    /* iMessage hint is Apple-only. */
    HU_ASSERT_TRUE(strstr(buf, "imessage") == NULL);
}

static void test_parsed_fail_emits_warning(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = TEST_PATH,
        .provider = "apple",
        .platform_is_apple = true,
        .already_exists = false,
        .parsed_ok = false,
    };
    char buf[1024];
    hu_error_t err = hu_onboard_nextstep_format(&ctx, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(
        buf, "Config written to " TEST_PATH ".\n"
             "Warning: config written but failed to parse — run 'human doctor --fix' to repair\n");
}

static void test_apple_success_emits_full_whats_next_block(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = TEST_PATH,
        .provider = "apple",
        .platform_is_apple = true,
        .already_exists = false,
        .parsed_ok = true,
    };
    char buf[1024];
    hu_error_t err = hu_onboard_nextstep_format(&ctx, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(buf, "Config verified OK\n"
                          "Config written to " TEST_PATH ".\n"
                          "What's next:\n"
                          "  1. Pair iMessage:  human doctor imessage\n"
                          "  2. Start the agent: human agent\n");
}

static void test_nonapple_success_omits_imessage_step(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = TEST_PATH,
        .provider = "gemini",
        .platform_is_apple = false,
        .already_exists = false,
        .parsed_ok = true,
    };
    char buf[1024];
    hu_error_t err = hu_onboard_nextstep_format(&ctx, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(buf, "Config verified OK\n"
                          "Config written to " TEST_PATH ".\n"
                          "What's next:\n"
                          "  1. Start the agent: human agent\n"
                          "  (Tier-1 channels other than iMessage require manual config — see "
                          "docs/guides/channels.md)\n");
    /* No iMessage step on non-Apple. */
    HU_ASSERT_TRUE(strstr(buf, "human doctor imessage") == NULL);
}

/* ------------------------------------------------------------------ */
/* Adversarial — OLD generic messages MUST be absent.                  */
/* These pin the bug we're fixing per .claude/rules/tests-that-pin-bugs.md. */
/* ------------------------------------------------------------------ */

static void test_apple_success_does_not_emit_old_generic_message(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = TEST_PATH,
        .provider = "apple",
        .platform_is_apple = true,
        .already_exists = false,
        .parsed_ok = true,
    };
    char buf[1024];
    hu_error_t err = hu_onboard_nextstep_format(&ctx, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    /* The old wizard tail printed this exact line — it MUST be gone. */
    HU_ASSERT_TRUE(strstr(buf, "Run 'human agent' to start chatting with Apple Intelligence.") ==
                   NULL);
    HU_ASSERT_TRUE(strstr(buf, "Run 'human agent' to start chatting.\n") == NULL);
}

static void test_nonapple_success_does_not_emit_old_generic_message(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = TEST_PATH,
        .provider = "gemini",
        .platform_is_apple = false,
        .already_exists = false,
        .parsed_ok = true,
    };
    char buf[1024];
    hu_error_t err = hu_onboard_nextstep_format(&ctx, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "Run 'human agent' to start chatting.\n") == NULL);
    HU_ASSERT_TRUE(strstr(buf, "Run 'human agent' to start chatting with Apple Intelligence.") ==
                   NULL);
}

static void test_already_exists_does_not_emit_old_bare_message(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = TEST_PATH,
        .provider = "gemini",
        .platform_is_apple = false,
        .already_exists = true,
        .parsed_ok = false,
    };
    char buf[1024];
    hu_error_t err = hu_onboard_nextstep_format(&ctx, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    /* Old line was: "Config already exists. Run 'human doctor' to check
     * status.\n" — no path, no imessage hint. The new line includes the
     * path. Asserting strcmp != 0 against the old buffer pins this. */
    HU_ASSERT_TRUE(strcmp(buf, "Config already exists. Run 'human doctor' to check status.\n") !=
                   0);
    /* And the new format embeds the path. */
    HU_ASSERT_TRUE(strstr(buf, TEST_PATH) != NULL);
}

/* ------------------------------------------------------------------ */
/* Error paths.                                                        */
/* ------------------------------------------------------------------ */

static void test_null_ctx_returns_invalid_argument(void) {
    char buf[64];
    hu_error_t err = hu_onboard_nextstep_format(NULL, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_null_out_returns_invalid_argument(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = TEST_PATH,
        .provider = "apple",
        .platform_is_apple = true,
        .already_exists = false,
        .parsed_ok = true,
    };
    hu_error_t err = hu_onboard_nextstep_format(&ctx, NULL, 64);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_zero_out_sz_returns_invalid_argument(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = TEST_PATH,
        .provider = "apple",
        .platform_is_apple = true,
        .already_exists = false,
        .parsed_ok = true,
    };
    char buf[1];
    hu_error_t err = hu_onboard_nextstep_format(&ctx, buf, 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_null_config_path_returns_invalid_argument(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = NULL,
        .provider = "apple",
        .platform_is_apple = true,
        .already_exists = false,
        .parsed_ok = true,
    };
    char buf[64];
    hu_error_t err = hu_onboard_nextstep_format(&ctx, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_truncation_returns_io_error(void) {
    hu_onboard_nextstep_ctx_t ctx = {
        .config_path = TEST_PATH,
        .provider = "apple",
        .platform_is_apple = true,
        .already_exists = false,
        .parsed_ok = true,
    };
    char buf[8]; /* far too small for any block */
    hu_error_t err = hu_onboard_nextstep_format(&ctx, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_ERR_IO);
}

/* ------------------------------------------------------------------ */
/* Suite registration.                                                 */
/* ------------------------------------------------------------------ */

void run_onboard_nextstep_tests(void) {
    HU_TEST_SUITE("Onboard nextstep formatter (US-9.2)");

    HU_RUN_TEST(test_already_exists_apple_shows_imessage_hint);
    HU_RUN_TEST(test_already_exists_nonapple_omits_imessage_hint);
    HU_RUN_TEST(test_parsed_fail_emits_warning);
    HU_RUN_TEST(test_apple_success_emits_full_whats_next_block);
    HU_RUN_TEST(test_nonapple_success_omits_imessage_step);

    HU_RUN_TEST(test_apple_success_does_not_emit_old_generic_message);
    HU_RUN_TEST(test_nonapple_success_does_not_emit_old_generic_message);
    HU_RUN_TEST(test_already_exists_does_not_emit_old_bare_message);

    HU_RUN_TEST(test_null_ctx_returns_invalid_argument);
    HU_RUN_TEST(test_null_out_returns_invalid_argument);
    HU_RUN_TEST(test_zero_out_sz_returns_invalid_argument);
    HU_RUN_TEST(test_null_config_path_returns_invalid_argument);
    HU_RUN_TEST(test_truncation_returns_io_error);
}
