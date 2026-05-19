/* tests/test_typedstream.c
 *
 * Phase 4 of docs/plans/2026-05-18-imessage-sota.md: pin the
 * attribute-run extractor at src/util/typedstream.c.
 *
 * Strategy: construct synthetic typedstream-style blobs that exercise
 * each attribute kind. The blobs are minimal — just the 0x01 0x2B text
 * segment plus, after the text, the attribute-key strings preceded by
 * range int markers (0x82 + 16-bit LE start + 0x82 + 16-bit LE length)
 * and (for string-valued attrs) followed by a length-prefixed value.
 *
 * The implementation's scanner uses BOUNDED forward/backward heuristics,
 * so synthetic fixtures are sufficient to validate every behavior
 * documented in include/human/util/typedstream.h. */

#include "human/channels/imessage_ingest.h"
#include "human/util/typedstream.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

/* Helper: write a 16-bit LE int marker (0x82) into a buffer slice. */
static size_t put_int16(unsigned char *buf, size_t at, int16_t v) {
    buf[at + 0] = 0x82;
    buf[at + 1] = (unsigned char)(v & 0xFF);
    buf[at + 2] = (unsigned char)((v >> 8) & 0xFF);
    return at + 3;
}

/* Helper: write a length-prefixed string (one byte length + bytes). */
static size_t put_lpstr(unsigned char *buf, size_t at, const char *s) {
    size_t n = strlen(s);
    buf[at++] = (unsigned char)n;
    memcpy(buf + at, s, n);
    return at + n;
}

/* Helper: write the 0x01 0x2B text segment with an inline length. */
static size_t put_text_segment(unsigned char *buf, size_t at, const char *text) {
    size_t n = strlen(text);
    buf[at++] = 0x01;
    buf[at++] = 0x2B;
    buf[at++] = (unsigned char)n;
    memcpy(buf + at, text, n);
    return at + n;
}

/* ── plain text, no attributes ────────────────────────────────────── */

static void plain_text_returns_text_zero_runs(void) {
    unsigned char blob[128] = {0};
    size_t n = put_text_segment(blob, 0, "Hello world");

    char text[64];
    hu_attribute_run_t runs[8];
    size_t runs_n = 99;
    hu_error_t err =
        hu_imessage_extract_attribute_runs(blob, n, text, sizeof(text), runs, 8, &runs_n);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_STR_EQ(text, "Hello world");
    HU_ASSERT_EQ((long long)runs_n, 0LL);
}

/* ── mention ──────────────────────────────────────────────────────── */

static void single_mention_extracts_handle(void) {
    unsigned char blob[256] = {0};
    size_t p = put_text_segment(blob, 0, "Hey @alice can you help");
    /* Range pair: start=4, length=6 (the "@alice" span) */
    p = put_int16(blob, p, 4);
    p = put_int16(blob, p, 6);
    /* Mention key + handle */
    const char *key = "__kIMMentionConfirmedMention";
    size_t klen = strlen(key);
    memcpy(blob + p, key, klen);
    p += klen;
    p = put_lpstr(blob, p, "alice@example.com");

    char text[128];
    hu_attribute_run_t runs[8];
    size_t runs_n = 0;
    hu_error_t err =
        hu_imessage_extract_attribute_runs(blob, p, text, sizeof(text), runs, 8, &runs_n);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_STR_EQ(text, "Hey @alice can you help");
    HU_ASSERT_GE((long long)runs_n, 1LL);
    HU_ASSERT_EQ((int)runs[0].kind, (int)HU_ATTR_MENTION);
    HU_ASSERT_STR_EQ(runs[0].detail, "alice@example.com");
}

/* ── link ─────────────────────────────────────────────────────────── */

static void link_span_detects_url(void) {
    unsigned char blob[256] = {0};
    size_t p = put_text_segment(blob, 0, "See https://h.com here");
    p = put_int16(blob, p, 4);
    p = put_int16(blob, p, 13);
    const char *key = "__kIMLinkAttributeName";
    memcpy(blob + p, key, strlen(key));
    p += strlen(key);
    p = put_lpstr(blob, p, "https://h.com");

    char text[128];
    hu_attribute_run_t runs[8];
    size_t runs_n = 0;
    hu_error_t err =
        hu_imessage_extract_attribute_runs(blob, p, text, sizeof(text), runs, 8, &runs_n);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_GE((long long)runs_n, 1LL);
    HU_ASSERT_EQ((int)runs[0].kind, (int)HU_ATTR_LINK);
    HU_ASSERT_STR_EQ(runs[0].detail, "https://h.com");
}

/* ── OTP ──────────────────────────────────────────────────────────── */

static void otp_attribute_detected(void) {
    unsigned char blob[256] = {0};
    size_t p = put_text_segment(blob, 0, "Your code is 123456");
    p = put_int16(blob, p, 13);
    p = put_int16(blob, p, 6);
    const char *key = "__kIMOneTimeCodeAttributeName";
    memcpy(blob + p, key, strlen(key));
    p += strlen(key);
    blob[p++] = 0x01; /* trailing inline truthy byte */

    char text[128];
    hu_attribute_run_t runs[8];
    size_t runs_n = 0;
    hu_error_t err =
        hu_imessage_extract_attribute_runs(blob, p, text, sizeof(text), runs, 8, &runs_n);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_GE((long long)runs_n, 1LL);
    HU_ASSERT_TRUE(hu_imessage_runs_contain_otp(runs, runs_n));
}

/* ── iOS 18 text effect ───────────────────────────────────────────── */

static void text_effect_big_extracted(void) {
    unsigned char blob[256] = {0};
    size_t p = put_text_segment(blob, 0, "WOW");
    p = put_int16(blob, p, 0);
    p = put_int16(blob, p, 3);
    const char *key = "__kIMTextEffectAttributeName";
    memcpy(blob + p, key, strlen(key));
    p += strlen(key);
    /* Effect code = 1 ("big"). Inline byte. */
    blob[p++] = 0x01;

    char text[64];
    hu_attribute_run_t runs[8];
    size_t runs_n = 0;
    hu_error_t err =
        hu_imessage_extract_attribute_runs(blob, p, text, sizeof(text), runs, 8, &runs_n);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_GE((long long)runs_n, 1LL);
    HU_ASSERT_EQ((int)runs[0].kind, (int)HU_ATTR_TEXT_EFFECT);
    HU_ASSERT_STR_EQ(runs[0].detail, "big");
}

/* ── multiple disjoint runs ────────────────────────────────────────── */

static void multiple_runs_emitted_and_sorted(void) {
    unsigned char blob[512] = {0};
    size_t p = put_text_segment(blob, 0, "Hi @bob see https://h.com");
    /* Link run at offset 11, length 13 — placed FIRST in the blob */
    p = put_int16(blob, p, 11);
    p = put_int16(blob, p, 13);
    const char *kl = "__kIMLinkAttributeName";
    memcpy(blob + p, kl, strlen(kl));
    p += strlen(kl);
    p = put_lpstr(blob, p, "https://h.com");
    /* Mention run at offset 3, length 4 — placed LATER in the blob */
    p = put_int16(blob, p, 3);
    p = put_int16(blob, p, 4);
    const char *km = "__kIMMentionConfirmedMention";
    memcpy(blob + p, km, strlen(km));
    p += strlen(km);
    p = put_lpstr(blob, p, "bob@example.com");

    char text[128];
    hu_attribute_run_t runs[8];
    size_t runs_n = 0;
    hu_error_t err =
        hu_imessage_extract_attribute_runs(blob, p, text, sizeof(text), runs, 8, &runs_n);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_GE((long long)runs_n, 2LL);
    /* Sorted by range_start: mention (3) comes before link (11). */
    HU_ASSERT_EQ((long long)runs[0].range_start, 3LL);
    HU_ASSERT_EQ((int)runs[0].kind, (int)HU_ATTR_MENTION);
    HU_ASSERT_EQ((long long)runs[1].range_start, 11LL);
    HU_ASSERT_EQ((int)runs[1].kind, (int)HU_ATTR_LINK);
}

/* ── synth_attributed_message: OTP returns 0 ──────────────────────── */

static void synth_skips_otp(void) {
    unsigned char blob[256] = {0};
    size_t p = put_text_segment(blob, 0, "Code 999111");
    p = put_int16(blob, p, 5);
    p = put_int16(blob, p, 6);
    const char *key = "__kIMOneTimeCodeAttributeName";
    memcpy(blob + p, key, strlen(key));
    p += strlen(key);
    blob[p++] = 0x01;

    char out[256];
    size_t n = hu_imessage_synth_attributed_message(blob, p, "alice@x", false, out, sizeof(out));
    HU_ASSERT_EQ((long long)n, 0LL);
}

/* ── synth_attributed_message: mention rendered inline ────────────── */

static void synth_includes_mention_inline(void) {
    unsigned char blob[256] = {0};
    size_t p = put_text_segment(blob, 0, "ping @bob");
    p = put_int16(blob, p, 5);
    p = put_int16(blob, p, 4);
    const char *key = "__kIMMentionConfirmedMention";
    memcpy(blob + p, key, strlen(key));
    p += strlen(key);
    p = put_lpstr(blob, p, "bob@x.com");

    char out[256];
    size_t n = hu_imessage_synth_attributed_message(blob, p, "alice@x", false, out, sizeof(out));
    HU_ASSERT_GT((long long)n, 0LL);
    HU_ASSERT_STR_CONTAINS(out, "ping @bob");
    HU_ASSERT_STR_CONTAINS(out, "(@bob@x.com)");
    HU_ASSERT_STR_CONTAINS(out, "alice@x");
}

/* ── synth_attributed_message: is_from_me uses first person ───────── */

static void synth_from_me_uses_first_person(void) {
    unsigned char blob[128] = {0};
    size_t p = put_text_segment(blob, 0, "yo");
    char out[128];
    size_t n = hu_imessage_synth_attributed_message(blob, p, "ignored", true, out, sizeof(out));
    HU_ASSERT_GT((long long)n, 0LL);
    HU_ASSERT_STR_CONTAINS(out, "I said");
    HU_ASSERT_STR_CONTAINS(out, "yo");
}

/* ── malformed inputs ─────────────────────────────────────────────── */

static void malformed_blob_returns_error(void) {
    unsigned char blob[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    char text[64];
    hu_attribute_run_t runs[8];
    size_t runs_n = 99;
    hu_error_t err = hu_imessage_extract_attribute_runs(blob, sizeof(blob), text, sizeof(text),
                                                        runs, 8, &runs_n);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

static void truncated_text_segment_returns_error(void) {
    /* 0x01 0x2B 0x05 + only 2 of 5 bytes */
    unsigned char blob[6] = {0x01, 0x2B, 0x05, 'A', 'B', 0};
    char text[64];
    hu_attribute_run_t runs[8];
    size_t runs_n = 0;
    hu_error_t err = hu_imessage_extract_attribute_runs(blob, sizeof(blob), text, sizeof(text),
                                                        runs, 8, &runs_n);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

static void empty_blob_returns_error(void) {
    char text[16];
    hu_attribute_run_t runs[4];
    size_t runs_n = 0;
    hu_error_t err =
        hu_imessage_extract_attribute_runs(NULL, 0, text, sizeof(text), runs, 4, &runs_n);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

static void null_args_return_error(void) {
    unsigned char blob[16] = {0};
    char text[16];
    hu_attribute_run_t runs[4];
    size_t runs_n = 0;
    hu_error_t err = hu_imessage_extract_attribute_runs(blob, sizeof(blob), NULL, sizeof(text),
                                                        runs, 4, &runs_n);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

static void text_capacity_overflow_reported(void) {
    unsigned char blob[64] = {0};
    size_t p = put_text_segment(blob, 0, "Hello world this is long");
    char text[4]; /* Too small */
    hu_attribute_run_t runs[4];
    size_t runs_n = 0;
    hu_error_t err =
        hu_imessage_extract_attribute_runs(blob, p, text, sizeof(text), runs, 4, &runs_n);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_LIMIT_REACHED);
}

/* ── runs_first_mention / runs_contain_otp on NULL ────────────────── */

static void helpers_null_safe(void) {
    HU_ASSERT_FALSE(hu_imessage_runs_contain_otp(NULL, 0));
    HU_ASSERT_NULL(hu_imessage_runs_first_mention(NULL, 0));

    hu_attribute_run_t r[1];
    r[0].kind = HU_ATTR_MENTION;
    r[0].range_start = 0;
    r[0].range_length = 4;
    strcpy(r[0].detail, "x");
    HU_ASSERT_NOT_NULL(hu_imessage_runs_first_mention(r, 1));
    HU_ASSERT_FALSE(hu_imessage_runs_contain_otp(r, 1));
}

/* ── runner ──────────────────────────────────────────────────────── */

void run_typedstream_tests(void) {
    HU_TEST_SUITE("typedstream");
    HU_RUN_TEST(plain_text_returns_text_zero_runs);
    HU_RUN_TEST(single_mention_extracts_handle);
    HU_RUN_TEST(link_span_detects_url);
    HU_RUN_TEST(otp_attribute_detected);
    HU_RUN_TEST(text_effect_big_extracted);
    HU_RUN_TEST(multiple_runs_emitted_and_sorted);
    HU_RUN_TEST(synth_skips_otp);
    HU_RUN_TEST(synth_includes_mention_inline);
    HU_RUN_TEST(synth_from_me_uses_first_person);
    HU_RUN_TEST(malformed_blob_returns_error);
    HU_RUN_TEST(truncated_text_segment_returns_error);
    HU_RUN_TEST(empty_blob_returns_error);
    HU_RUN_TEST(null_args_return_error);
    HU_RUN_TEST(text_capacity_overflow_reported);
    HU_RUN_TEST(helpers_null_safe);
}
