/* `human memory search` prints each hit with a 2000-byte ceiling. The ceiling
 * must never split a multi-byte UTF-8 sequence: on 2026-09-03 the live
 * semantic gate died reading this output with "invalid continuation byte"
 * (fixed consumer-side in 9a70a296b; this pins the producer side). */
#include "human/cli_commands.h"
#include "test_framework.h"
#include <string.h>

#define CEIL 2000u

static void test_print_len_ascii_under_ceiling_is_untouched(void) {
    HU_ASSERT_EQ((long)hu_cli_memory_print_len("hello", 5), 5L);
    char s[CEIL];
    memset(s, 'a', sizeof(s));
    HU_ASSERT_EQ((long)hu_cli_memory_print_len(s, CEIL), (long)CEIL);
    HU_ASSERT_EQ((long)hu_cli_memory_print_len(NULL, 5), 0L);
}

static void test_print_len_never_splits_multibyte_char_at_ceiling(void) {
    /* 2100 bytes of 'a' with a 3-byte U+2019 spanning bytes 1999..2001, so
     * a raw 2000 cut would emit the lead byte 0xE2 without its tail. */
    char s[2100];
    memset(s, 'a', sizeof(s));
    memcpy(s + 1999, "\xe2\x80\x99", 3);
    size_t cut = hu_cli_memory_print_len(s, sizeof(s));
    HU_ASSERT_TRUE(cut <= CEIL);
    HU_ASSERT_TRUE(cut < 2000);
    HU_ASSERT_TRUE(((unsigned char)s[cut] & 0xC0u) != 0x80u); /* ends on a full char */
    HU_ASSERT_EQ((long)cut, 1999L);
    /* A sequence that ENDS exactly at the ceiling is kept whole. */
    memset(s, 'a', sizeof(s));
    memcpy(s + 1997, "\xe2\x80\x99", 3);
    HU_ASSERT_EQ((long)hu_cli_memory_print_len(s, sizeof(s)), (long)CEIL);
}

void run_cli_commands_memory_print_tests(void) {
    HU_TEST_SUITE("cli_commands_memory_print");
    HU_RUN_TEST(test_print_len_ascii_under_ceiling_is_untouched);
    HU_RUN_TEST(test_print_len_never_splits_multibyte_char_at_ceiling);
}
