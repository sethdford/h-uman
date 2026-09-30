/* `human memory search` prints each hit with a 2000-byte ceiling. The ceiling
 * must never split a multi-byte UTF-8 sequence: on 2026-09-03 the live
 * semantic gate died reading this output with "invalid continuation byte"
 * (fixed consumer-side in 9a70a296b; this pins the producer side). */
#include "human/agent/graph_grounding.h"
#include "human/cli_commands.h"
#include "test_framework.h"
#include <stdio.h>
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

static void test_reindex_args_trailing_full_is_seen(void) {
    char *argv1[] = {"human", "memory", "reindex", "--full"};
    size_t lim = 99;
    bool full = false;
    hu_cli_parse_reindex_args(4, argv1, &lim, &full);
    HU_ASSERT_TRUE(full); /* the 2026-09-20 loop bound dropped this */
    HU_ASSERT_EQ(lim, (size_t)0);
    char *argv2[] = {"human", "memory", "reindex", "--limit", "250", "--full"};
    hu_cli_parse_reindex_args(6, argv2, &lim, &full);
    HU_ASSERT_TRUE(full);
    HU_ASSERT_EQ(lim, (size_t)250);
    char *argv3[] = {"human", "memory", "reindex", "--limit"}; /* dangling value */
    hu_cli_parse_reindex_args(4, argv3, &lim, &full);
    HU_ASSERT_FALSE(full);
    HU_ASSERT_EQ(lim, (size_t)0);
    hu_cli_parse_reindex_args(3, argv3, &lim, &full);
    HU_ASSERT_FALSE(full);
}

static void test_ground_args_plain_and_full(void) {
    const char *contact = NULL, *msg = NULL;
    bool full = true;
    char *a1[] = {"human", "memory", "ground", "+15550000001", "hi there"};
    HU_ASSERT_TRUE(hu_cli_parse_ground_args(5, a1, &contact, &msg, &full));
    HU_ASSERT_STR_EQ(contact, "+15550000001");
    HU_ASSERT_STR_EQ(msg, "hi there");
    HU_ASSERT_FALSE(full);
    char *a2[] = {"human", "memory", "ground", "--full", "+15550000001", "hi there"};
    HU_ASSERT_TRUE(hu_cli_parse_ground_args(6, a2, &contact, &msg, &full));
    HU_ASSERT_TRUE(full);
    HU_ASSERT_STR_EQ(contact, "+15550000001");
    HU_ASSERT_STR_EQ(msg, "hi there");
    HU_ASSERT_FALSE(hu_cli_parse_ground_args(5, a2, &contact, &msg, &full)); /* --full, no msg */
    HU_ASSERT_FALSE(hu_cli_parse_ground_args(4, a1, &contact, &msg, &full));
    char *a3[] = {"human", "memory", "ground", "", "hi"};
    HU_ASSERT_FALSE(hu_cli_parse_ground_args(5, a3, &contact, &msg, &full));
}

static void read_all(FILE *f, char *buf, size_t cap) {
    rewind(f);
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
}

static void test_ground_emit_full_prints_turn_stats_then_block(void) {
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_graph_ground_turn_stats_t st;
    memset(&st, 0, sizeof(st));
    st.matched_entities = 2;
    st.via_fallback = true;
    st.typed_names = 1;
    const char *blk = "- Salim (person)\n";
    hu_cli_memory_ground_emit(f, true, 0, &st, blk, strlen(blk));
    char buf[256];
    read_all(f, buf, sizeof(buf));
    fclose(f);
    HU_ASSERT_STR_EQ(buf, "matched=2 bytes=17 fallback=1 self=0 names=1\n- Salim (person)\n\n");
}

static void test_ground_emit_plain_keeps_the_old_shape(void) {
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_cli_memory_ground_emit(f, false, 3, NULL, NULL, 0);
    char buf[64];
    read_all(f, buf, sizeof(buf));
    fclose(f);
    HU_ASSERT_STR_EQ(buf, "matched=3 bytes=0\n");
}

void run_cli_commands_memory_print_tests(void) {
    HU_TEST_SUITE("cli_commands_memory_print");
    HU_RUN_TEST(test_print_len_ascii_under_ceiling_is_untouched);
    HU_RUN_TEST(test_print_len_never_splits_multibyte_char_at_ceiling);
    HU_RUN_TEST(test_reindex_args_trailing_full_is_seen);
    HU_RUN_TEST(test_ground_args_plain_and_full);
    HU_RUN_TEST(test_ground_emit_full_prints_turn_stats_then_block);
    HU_RUN_TEST(test_ground_emit_plain_keeps_the_old_shape);
}
