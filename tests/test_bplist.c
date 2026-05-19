/* tests/test_bplist.c
 *
 * Pin every parser path in src/util/bplist.c. Phase 3 keystone tests
 * for docs/plans/2026-05-18-imessage-sota.md.
 *
 * Strategy: construct bplist00 blobs as literal byte arrays and verify
 * each accessor. The fixtures are small and hand-traced so failures
 * point at the exact bug shape, not a generic "parse failed". */

#include "human/util/bplist.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

/* Build a 32-byte trailer. offset_size and ref_size both 1 (sufficient
 * for fixtures under 256 bytes and 256 objects). */
#define HU_BPL_TRAILER_NUM_TOP_OFF(num, top, off_tbl)                                        \
    /* 6 unused */ 0, 0, 0, 0, 0, 0, /* offset_int_size */ 0x01, /* object_ref_size */ 0x01, \
        /* num_objects (u64 BE) */ 0, 0, 0, 0, 0, 0, 0, (unsigned char)(num),                \
        /* top_object   (u64 BE) */ 0, 0, 0, 0, 0, 0, 0, (unsigned char)(top),               \
        /* offset_table (u64 BE) */ 0, 0, 0, 0, 0, 0, 0, (unsigned char)(off_tbl)

/* ── magic & trailer validation ───────────────────────────────────── */

static void test_parse_rejects_null_input(void) {
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(NULL, 64, &p), (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(p);
}

static void test_parse_rejects_too_short(void) {
    unsigned char tiny[] = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(tiny, sizeof(tiny), &p), (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(p);
}

static void test_parse_rejects_wrong_magic(void) {
    /* bplist01: a real (writeable) magic for newer formats. The
     * read-only parser must reject it because it cannot honor v1+
     * layout differences. */
    unsigned char blob[8 + 32] = {'b', 'p', 'l', 'i', 's', 't', '0', '1'};
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(p);
}

static void test_parse_rejects_garbage(void) {
    unsigned char blob[50] = {0};
    memcpy(blob, "XXXXXXXX", 8);
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_parse_rejects_oob_top_object(void) {
    /* Header + one int + offset table + trailer claiming top=5 but
     * num_objects=1. */
    unsigned char blob[] = {
        'b',
        'p',
        'l',
        'i',
        's',
        't',
        '0',
        '0',
        0x10,
        0x2A, /* int 42 at offset 8 */
        0x08, /* offset table at offset 10 */
        HU_BPL_TRAILER_NUM_TOP_OFF(1, 5, 10),
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_ERR_INVALID_ARGUMENT);
}

/* ── primitive accessors ──────────────────────────────────────────── */

static void test_int_root_returns_value(void) {
    unsigned char blob[] = {
        'b',
        'p',
        'l',
        'i',
        's',
        't',
        '0',
        '0',
        0x10,
        0x2A, /* int 42 */
        0x08, /* offset table */
        HU_BPL_TRAILER_NUM_TOP_OFF(1, 0, 10),
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_OK);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_EQ((int)hu_bplist_kind(p, hu_bplist_root(p)), (int)HU_BPLIST_INT);
    HU_ASSERT_EQ((long long)hu_bplist_get_int(p, hu_bplist_root(p)), 42LL);
    hu_bplist_free(p);
}

static void test_bool_true_and_false(void) {
    /* Two objects: bool true (0x09) at off 8, bool false (0x08) at off 9. */
    unsigned char blob[] = {
        'b',
        'p',
        'l',
        'i',
        's',
        't',
        '0',
        '0',
        0x09, /* true */
        0x08, /* false */
        0x08,
        0x09, /* offset table: obj0@8, obj1@9 */
        HU_BPL_TRAILER_NUM_TOP_OFF(2, 0, 10),
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_OK);
    HU_ASSERT_EQ((int)hu_bplist_kind(p, 0), (int)HU_BPLIST_BOOL);
    HU_ASSERT_EQ((int)hu_bplist_kind(p, 1), (int)HU_BPLIST_BOOL);
    HU_ASSERT_TRUE(hu_bplist_get_bool(p, 0));
    HU_ASSERT_FALSE(hu_bplist_get_bool(p, 1));
    hu_bplist_free(p);
}

static void test_real_double_roundtrips(void) {
    /* IEEE 754 binary64 for 3.14 = 0x40091EB851EB851F (BE). */
    unsigned char blob[] = {
        'b',
        'p',
        'l',
        'i',
        's',
        't',
        '0',
        '0',
        0x23, /* real, 2^3=8 bytes */
        0x40,
        0x09,
        0x1E,
        0xB8,
        0x51,
        0xEB,
        0x85,
        0x1F,
        0x08, /* offset table */
        HU_BPL_TRAILER_NUM_TOP_OFF(1, 0, 17),
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_OK);
    HU_ASSERT_EQ((int)hu_bplist_kind(p, 0), (int)HU_BPLIST_REAL);
    HU_ASSERT_FLOAT_EQ(hu_bplist_get_real(p, 0), 3.14, 1e-9);
    hu_bplist_free(p);
}

static void test_ascii_string_decodes(void) {
    unsigned char blob[] = {
        'b',
        'p',
        'l',
        'i',
        's',
        't',
        '0',
        '0',
        0x55,
        'h',
        'e',
        'l',
        'l',
        'o',  /* ASCII str len=5 */
        0x08, /* offset table */
        HU_BPL_TRAILER_NUM_TOP_OFF(1, 0, 14),
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_OK);
    HU_ASSERT_EQ((int)hu_bplist_kind(p, 0), (int)HU_BPLIST_STRING);
    char out[16] = {0};
    size_t n = hu_bplist_get_string(p, 0, out, sizeof(out));
    HU_ASSERT_EQ((long long)n, 5LL);
    HU_ASSERT_STR_EQ(out, "hello");
    hu_bplist_free(p);
}

static void test_utf16_string_decodes_to_utf8(void) {
    /* UTF-16BE "Hi" = 0x0048 0x0069, ASCII "Hi" in UTF-8 = "Hi" */
    unsigned char blob[] = {
        'b',
        'p',
        'l',
        'i',
        's',
        't',
        '0',
        '0',
        0x62,
        0x00,
        0x48,
        0x00,
        0x69, /* UTF-16BE len=2 */
        0x08, /* offset table */
        HU_BPL_TRAILER_NUM_TOP_OFF(1, 0, 13),
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_OK);
    char out[16] = {0};
    HU_ASSERT_EQ((long long)hu_bplist_get_string(p, 0, out, sizeof(out)), 2LL);
    HU_ASSERT_STR_EQ(out, "Hi");
    hu_bplist_free(p);
}

static void test_data_blob_returns_pointer_and_length(void) {
    unsigned char blob[] = {
        'b',
        'p',
        'l',
        'i',
        's',
        't',
        '0',
        '0',
        0x43,
        0xAA,
        0xBB,
        0xCC, /* data len=3 */
        0x08, /* offset table */
        HU_BPL_TRAILER_NUM_TOP_OFF(1, 0, 12),
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_OK);
    HU_ASSERT_EQ((int)hu_bplist_kind(p, 0), (int)HU_BPLIST_DATA);
    size_t dlen = 0;
    const unsigned char *d = hu_bplist_get_data(p, 0, &dlen);
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_EQ((long long)dlen, 3LL);
    HU_ASSERT_EQ((int)d[0], 0xAA);
    HU_ASSERT_EQ((int)d[1], 0xBB);
    HU_ASSERT_EQ((int)d[2], 0xCC);
    hu_bplist_free(p);
}

/* ── arrays ───────────────────────────────────────────────────────── */

static void test_array_of_two_strings(void) {
    /* obj0 = array[2] of refs (1, 2)
     * obj1 = "a"
     * obj2 = "b"
     */
    unsigned char blob[] = {
        'b',
        'p',
        'l',
        'i',
        's',
        't',
        '0',
        '0',
        0xA2,
        0x01,
        0x02, /* array len=2, refs 1,2  @8 */
        0x51,
        'a', /* str "a"                @11 */
        0x51,
        'b', /* str "b"                @13 */
        0x08,
        0x0B,
        0x0D, /* offset table           @15 */
        HU_BPL_TRAILER_NUM_TOP_OFF(3, 0, 15),
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_OK);
    HU_ASSERT_EQ((int)hu_bplist_kind(p, 0), (int)HU_BPLIST_ARRAY);
    HU_ASSERT_EQ((long long)hu_bplist_array_count(p, 0), 2LL);
    char out[8] = {0};
    HU_ASSERT_EQ((long long)hu_bplist_get_string(p, hu_bplist_array_at(p, 0, 0), out, sizeof(out)),
                 1LL);
    HU_ASSERT_STR_EQ(out, "a");
    HU_ASSERT_EQ((long long)hu_bplist_get_string(p, hu_bplist_array_at(p, 0, 1), out, sizeof(out)),
                 1LL);
    HU_ASSERT_STR_EQ(out, "b");
    hu_bplist_free(p);
}

/* ── dicts ────────────────────────────────────────────────────────── */

static void test_dict_lookup_hit_and_miss(void) {
    /* obj0 = dict{key1: val1}
     * obj1 = "k"
     * obj2 = "v"
     * Dict body layout: [key refs ...][value refs ...]
     */
    unsigned char blob[] = {
        'b',
        'p',
        'l',
        'i',
        's',
        't',
        '0',
        '0',
        0xD1,
        0x01,
        0x02, /* dict count=1, keys[1], vals[2]  @8 */
        0x51,
        'k', /* str "k"                          @11 */
        0x51,
        'v', /* str "v"                          @13 */
        0x08,
        0x0B,
        0x0D, /* offset table                     @15 */
        HU_BPL_TRAILER_NUM_TOP_OFF(3, 0, 15),
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_OK);
    HU_ASSERT_EQ((int)hu_bplist_kind(p, 0), (int)HU_BPLIST_DICT);
    HU_ASSERT_EQ((long long)hu_bplist_dict_count(p, 0), 1LL);
    size_t v = hu_bplist_dict_lookup(p, 0, "k");
    HU_ASSERT_EQ((long long)v, 2LL);
    char out[8] = {0};
    HU_ASSERT_EQ((long long)hu_bplist_get_string(p, v, out, sizeof(out)), 1LL);
    HU_ASSERT_STR_EQ(out, "v");

    /* Miss returns SIZE_MAX. */
    HU_ASSERT_TRUE(hu_bplist_dict_lookup(p, 0, "nope") == (size_t)-1);
    hu_bplist_free(p);
}

/* ── path walker ──────────────────────────────────────────────────── */

static void test_nested_dict_path_walks_three_levels(void) {
    /* Build:
     *   root = { "ec": { "0": [ { "t": "hello" } ] } }
     *
     * Object layout (chosen so refs fit in 1 byte):
     *   obj0  = root dict { "ec" -> obj1 }
     *   obj1  = inner dict { "0"  -> obj2 }
     *   obj2  = array [ obj3 ]
     *   obj3  = leaf dict { "t" -> obj4 }
     *   obj4  = string "hello"
     *   obj5  = string "ec"
     *   obj6  = string "0"
     *   obj7  = string "t"
     */
    unsigned char blob[] = {
        'b',
        'p',
        'l',
        'i',
        's',
        't',
        '0',
        '0',
        /* @08 obj0: dict count=1 keys=[5] vals=[1] */
        0xD1,
        0x05,
        0x01,
        /* @11 obj1: dict count=1 keys=[6] vals=[2] */
        0xD1,
        0x06,
        0x02,
        /* @14 obj2: array count=1 [3] */
        0xA1,
        0x03,
        /* @16 obj3: dict count=1 keys=[7] vals=[4] */
        0xD1,
        0x07,
        0x04,
        /* @19 obj4: string "hello" */
        0x55,
        'h',
        'e',
        'l',
        'l',
        'o',
        /* @25 obj5: string "ec" */
        0x52,
        'e',
        'c',
        /* @28 obj6: string "0" */
        0x51,
        '0',
        /* @30 obj7: string "t" */
        0x51,
        't',
        /* @32 offset table: 8 entries (1 byte each) */
        0x08,
        0x0B,
        0x0E,
        0x10,
        0x13,
        0x19,
        0x1C,
        0x1E,
        /* @40 trailer (32 bytes) */
        HU_BPL_TRAILER_NUM_TOP_OFF(8, 0, 32),
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_OK);
    HU_ASSERT_NOT_NULL(p);
    char out[32] = {0};
    /* Path: root_dict["ec"] -> dict["0"] -> array[0] -> dict["t"] -> "hello".
     * The second "0" is a NUMERIC array index, dispatched on the array
     * node's kind; the first "0" is a dict key. */
    const char *path[] = {"ec", "0", "0", "t", NULL};
    size_t n = hu_bplist_get_string_at_path(p, path, out, sizeof(out));
    HU_ASSERT_EQ((long long)n, 5LL);
    HU_ASSERT_STR_EQ(out, "hello");

    /* Missing path returns 0 and leaves out empty-string-terminated. */
    const char *bad_path[] = {"ec", "0", "0", "missing", NULL};
    out[0] = 'x';
    out[1] = '\0';
    HU_ASSERT_EQ((long long)hu_bplist_get_string_at_path(p, bad_path, out, sizeof(out)), 0LL);
    HU_ASSERT_EQ((int)out[0], (int)'\0');

    hu_bplist_free(p);
}

/* ── defensive: malformed input ───────────────────────────────────── */

static void test_truncated_trailer_rejected(void) {
    /* Header + body + offset table but trailer only 20 bytes. */
    unsigned char blob[8 + 2 + 1 + 20];
    memcpy(blob, "bplist00", 8);
    blob[8] = 0x10;
    blob[9] = 0x01;
    blob[10] = 0x08;
    memset(blob + 11, 0, 20);
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_offset_table_pointing_into_trailer_rejected(void) {
    /* offset_table_offset = len-30 puts it INSIDE the trailer. */
    unsigned char blob[] = {
        'b',  'p',  'l',  'i',
        's',  't',  '0',  '0',
        0x10, 0x01, 0x08, HU_BPL_TRAILER_NUM_TOP_OFF(1, 0, 80), /* off table claim beyond data */
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_type_mismatch_accessors_return_zero(void) {
    /* Root is an int; calling string accessor returns 0. */
    unsigned char blob[] = {
        'b', 'p', 'l',  'i',  's',  't',
        '0', '0', 0x10, 0x07, 0x08, HU_BPL_TRAILER_NUM_TOP_OFF(1, 0, 10),
    };
    hu_bplist_t *p = NULL;
    HU_ASSERT_EQ((int)hu_bplist_parse(blob, sizeof(blob), &p), (int)HU_OK);
    char out[8] = {0};
    HU_ASSERT_EQ((long long)hu_bplist_get_string(p, 0, out, sizeof(out)), 0LL);
    HU_ASSERT_EQ((long long)hu_bplist_array_count(p, 0), 0LL);
    HU_ASSERT_EQ((long long)hu_bplist_dict_count(p, 0), 0LL);
    HU_ASSERT_FALSE(hu_bplist_get_bool(p, 0));
    HU_ASSERT_FLOAT_EQ(hu_bplist_get_real(p, 0), 0.0, 1e-9);
    hu_bplist_free(p);
}

static void test_free_handles_null(void) {
    hu_bplist_free(NULL);
    /* Survival = success. */
}

/* ── runner ───────────────────────────────────────────────────────── */

void run_bplist_tests(void) {
    HU_TEST_SUITE("bplist");
    HU_RUN_TEST(test_parse_rejects_null_input);
    HU_RUN_TEST(test_parse_rejects_too_short);
    HU_RUN_TEST(test_parse_rejects_wrong_magic);
    HU_RUN_TEST(test_parse_rejects_garbage);
    HU_RUN_TEST(test_parse_rejects_oob_top_object);
    HU_RUN_TEST(test_int_root_returns_value);
    HU_RUN_TEST(test_bool_true_and_false);
    HU_RUN_TEST(test_real_double_roundtrips);
    HU_RUN_TEST(test_ascii_string_decodes);
    HU_RUN_TEST(test_utf16_string_decodes_to_utf8);
    HU_RUN_TEST(test_data_blob_returns_pointer_and_length);
    HU_RUN_TEST(test_array_of_two_strings);
    HU_RUN_TEST(test_dict_lookup_hit_and_miss);
    HU_RUN_TEST(test_nested_dict_path_walks_three_levels);
    HU_RUN_TEST(test_truncated_trailer_rejected);
    HU_RUN_TEST(test_offset_table_pointing_into_trailer_rejected);
    HU_RUN_TEST(test_type_mismatch_accessors_return_zero);
    HU_RUN_TEST(test_free_handles_null);
}
