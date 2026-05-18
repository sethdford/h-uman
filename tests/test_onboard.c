/* tests/test_onboard.c — US-43.2 onboard nextstep formatter contract pin.
 *
 * Pure formatter; no I/O, no syscalls, no `HU_IS_TEST` guard needed.
 * 10 tests map the 5 ACs of US-43.2 (see sprints/sprint-43/stories.md)
 * plus hardening + round-trip assertions. The adversarial assertions
 * pin OLD-text ABSENT via `strstr(...) == NULL` so a future change that
 * accidentally reintroduces "You're all set" (or, in `all_ready`, the
 * forbidden "setup"/"configure" tokens) fails loudly. */
#include "human/core/error.h"
#include "human/onboard.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AC-43.2.1: all-ready output contains "human chat" AND must NOT contain
 * "setup" or "configure". */
static void nextstep_all_ready_contains_human_chat_no_setup_no_configure(void) {
    char buf[256];
    hu_error_t rc =
        hu_onboard_nextstep_format(/*imessage_paired*/ true, /*persona_set*/ true,
                                   /*ollama_ok*/ true, /*brew_installed*/ true, buf, sizeof(buf));
    HU_ASSERT_EQ(rc, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "human chat") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "setup") == NULL);
    HU_ASSERT_TRUE(strstr(buf, "configure") == NULL);
}

/* AC-43.2.2 (half): fallback_bare must be strcmp-distinct from every
 * other variant. */
static void nextstep_fallback_bare_strcmp_distinct_from_other_four(void) {
    char fallback[256], pair[256], cloud[256], no_brew[256], all_ready[256];
    HU_ASSERT_EQ(hu_onboard_nextstep_format(false, false, false, false, fallback, sizeof(fallback)),
                 HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(false, true, false, false, pair, sizeof(pair)), HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, false, false, cloud, sizeof(cloud)), HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, true, false, no_brew, sizeof(no_brew)),
                 HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, true, true, all_ready, sizeof(all_ready)),
                 HU_OK);
    HU_ASSERT_TRUE(strcmp(fallback, pair) != 0);
    HU_ASSERT_TRUE(strcmp(fallback, cloud) != 0);
    HU_ASSERT_TRUE(strcmp(fallback, no_brew) != 0);
    HU_ASSERT_TRUE(strcmp(fallback, all_ready) != 0);
}

/* AC-43.2.3: short-buffer returns HU_ERR_IO and writes a NUL terminator.
 * Design decision: HU_ERR_BUFFER_TOO_SMALL is not in hu_error_t; we reuse
 * HU_ERR_IO. See sprints/sprint-43/designs/US-43.2.md "Design Decision". */
static void nextstep_buffer_one_byte_short_returns_HU_ERR_IO(void) {
    /* First measure the required size for the all_ready output. */
    char big[256];
    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, true, true, big, sizeof(big)), HU_OK);
    size_t need = strlen(big) + 1;
    HU_ASSERT_TRUE(need > 1);

    /* Now call with exactly one byte too few. */
    char small[256];
    /* Sentinel that must be overwritten with NUL (or never read past). */
    memset(small, 0x5a, sizeof(small));
    hu_error_t rc = hu_onboard_nextstep_format(true, true, true, true, small, need - 1);
    HU_ASSERT_EQ(rc, HU_ERR_IO);
    /* See sprints/sprint-43/designs/US-43.2.md "Design Decision" — short
     * buffer surfaces as HU_ERR_IO, not the AC's literally-named
     * HU_ERR_BUFFER_TOO_SMALL (that enum doesn't exist). */
    HU_ASSERT_EQ(small[0], '\0');
}

/* AC-43.2.4: NONE of the five outputs contains the legacy generic
 * "You're all set" string. Five separate negative pins. */
static void nextstep_no_output_matches_old_youre_all_set_text(void) {
    char buf[256];

    HU_ASSERT_EQ(hu_onboard_nextstep_format(false, false, false, false, buf, sizeof(buf)), HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "You're all set") == NULL);

    HU_ASSERT_EQ(hu_onboard_nextstep_format(false, true, false, false, buf, sizeof(buf)), HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "You're all set") == NULL);

    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, false, false, buf, sizeof(buf)), HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "You're all set") == NULL);

    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, true, false, buf, sizeof(buf)), HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "You're all set") == NULL);

    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, true, true, buf, sizeof(buf)), HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "You're all set") == NULL);
}

/* AC-43.2.2 (other half): all 5 distinct outputs pairwise strcmp-distinct. */
static void nextstep_all_five_outputs_strcmp_distinct(void) {
    char outputs[5][256];
    HU_ASSERT_EQ(hu_onboard_nextstep_format(false, false, false, false, outputs[0], 256), HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(false, true, false, false, outputs[1], 256), HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, false, false, outputs[2], 256), HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, true, false, outputs[3], 256), HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, true, true, outputs[4], 256), HU_OK);
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            HU_ASSERT_TRUE(strcmp(outputs[i], outputs[j]) != 0);
        }
    }
}

/* Hardening: NULL buf returns HU_ERR_INVALID_ARGUMENT and does not crash. */
static void nextstep_null_buf_returns_HU_ERR_INVALID_ARGUMENT_no_crash(void) {
    hu_error_t rc = hu_onboard_nextstep_format(true, true, true, true, NULL, 256);
    HU_ASSERT_EQ(rc, HU_ERR_INVALID_ARGUMENT);

    /* Also: zero buflen with a real pointer → same error code. */
    char buf[8];
    rc = hu_onboard_nextstep_format(true, true, true, true, buf, 0);
    HU_ASSERT_EQ(rc, HU_ERR_INVALID_ARGUMENT);
}

/* Hardening: buf[0] is NUL-terminated even on short-buffer truncation
 * (so callers can safely fputs/printf without checking the rc). */
static void nextstep_buf_always_nul_terminated_even_on_truncation(void) {
    char buf[4];
    memset(buf, 0x42, sizeof(buf)); /* poison */
    hu_error_t rc = hu_onboard_nextstep_format(true, true, true, true, buf, sizeof(buf));
    HU_ASSERT_EQ(rc, HU_ERR_IO);
    HU_ASSERT_EQ(buf[0], '\0');
}

/* AC-43.2.1 sibling: pair_imessage output mentions the pair command. */
static void nextstep_pair_imessage_output_mentions_pair_command(void) {
    char buf[256];
    hu_error_t rc =
        hu_onboard_nextstep_format(/*imessage_paired*/ false, /*persona_set*/ true,
                                   /*ollama_ok*/ true, /*brew_installed*/ true, buf, sizeof(buf));
    HU_ASSERT_EQ(rc, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "human channel pair imessage") != NULL);
}

/* AC-43.2.1 sibling: chat_cloud output mentions "Cloud" (cloud provider
 * configured branch when ollama is not reachable). */
static void nextstep_chat_cloud_output_mentions_cloud(void) {
    char buf[256];
    hu_error_t rc =
        hu_onboard_nextstep_format(/*imessage_paired*/ true, /*persona_set*/ true,
                                   /*ollama_ok*/ false, /*brew_installed*/ false, buf, sizeof(buf));
    HU_ASSERT_EQ(rc, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "Cloud") != NULL || strstr(buf, "cloud") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "human chat") != NULL);
}

/* AC-43.2.5: round-trip via the precedence ladder — every valid input
 * combination produces exactly one of the 5 canonical outputs. */
static void nextstep_round_trip_call_site_onboard_run_emits_one_of_five(void) {
    /* Capture all 5 canonical outputs. */
    char canonical[5][256];
    HU_ASSERT_EQ(hu_onboard_nextstep_format(false, false, false, false, canonical[0], 256), HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(false, true, false, false, canonical[1], 256), HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, false, false, canonical[2], 256), HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, true, false, canonical[3], 256), HU_OK);
    HU_ASSERT_EQ(hu_onboard_nextstep_format(true, true, true, true, canonical[4], 256), HU_OK);

    /* Iterate every 4-bool combination; assert each output strcmp-matches
     * exactly one canonical output. */
    for (int bits = 0; bits < 16; bits++) {
        bool im = (bits & 1) != 0;
        bool ps = (bits & 2) != 0;
        bool ol = (bits & 4) != 0;
        bool br = (bits & 8) != 0;
        char buf[256];
        hu_error_t rc = hu_onboard_nextstep_format(im, ps, ol, br, buf, sizeof(buf));
        HU_ASSERT_EQ(rc, HU_OK);
        int match_count = 0;
        for (int k = 0; k < 5; k++) {
            if (strcmp(buf, canonical[k]) == 0)
                match_count++;
        }
        HU_ASSERT_EQ(match_count, 1);
    }
}

void run_onboard_tests(void);
void run_onboard_tests(void) {
    HU_TEST_SUITE("onboard_nextstep");
    HU_RUN_TEST(nextstep_all_ready_contains_human_chat_no_setup_no_configure);
    HU_RUN_TEST(nextstep_fallback_bare_strcmp_distinct_from_other_four);
    HU_RUN_TEST(nextstep_buffer_one_byte_short_returns_HU_ERR_IO);
    HU_RUN_TEST(nextstep_no_output_matches_old_youre_all_set_text);
    HU_RUN_TEST(nextstep_all_five_outputs_strcmp_distinct);
    HU_RUN_TEST(nextstep_null_buf_returns_HU_ERR_INVALID_ARGUMENT_no_crash);
    HU_RUN_TEST(nextstep_buf_always_nul_terminated_even_on_truncation);
    HU_RUN_TEST(nextstep_pair_imessage_output_mentions_pair_command);
    HU_RUN_TEST(nextstep_chat_cloud_output_mentions_cloud);
    HU_RUN_TEST(nextstep_round_trip_call_site_onboard_run_emits_one_of_five);
}
