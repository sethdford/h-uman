/* tests/test_imessage_balloon_decode.c
 *
 * Phase 5 of docs/plans/2026-05-18-imessage-sota.md.
 *
 * Builds synthetic bplist00 blobs (using the same byte-construction
 * pattern as tests/test_bplist.c) and verifies each decoder pulls the
 * right detail string out — INCLUDING privacy contracts:
 *   - Apple Pay decoder must NEVER emit a digit run > 2 chars or
 *     "$" / "USD" markers, even when given amount keys.
 *   - Placemark decoder must NEVER emit lat/lon, even when given them.
 *
 * The privacy contract is also STRUCTURAL: the decoder code in
 * src/channels/imessage_balloon_decode.c contains no reference to
 * "amount" / "currency" / "value" / "latitude" / "longitude" — these
 * tests pin behavioral compliance on top. */

#include "test_framework.h"

#ifdef HU_HAS_IMESSAGE

#include "human/channels/imessage_balloon_decode.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

/* ── bplist builder helpers (minimal, copy of test_bplist.c approach) ─
 *
 * We only need to build dicts of short ASCII strings, which is the
 * Apple plist shape for these balloon types. Format details:
 *
 *   0xD<n>     dict header with n entries (n < 0x10)
 *   0x5<n>     ASCII string with n bytes (n < 0x10)
 *   0x6<n>     UTF-16 string (we use ASCII so we always emit 0x5)
 *
 * Trailer (32 bytes at end): offsets are 1-byte if blob is small enough.
 * For our test fixtures < 256 bytes, offset_int_size = 1 and ref_size = 1. */

/* Build a bplist00 blob encoding a top-level dict of {key: value} ASCII
 * string pairs. `entries` is an array of NULL-terminated alternating
 * key/value pointers; `n_pairs` is the pair count.
 *
 * Returns bytes written to `out`. Caller sizes `out` large enough.
 * Designed for small fixtures (single-byte offsets). */
static size_t build_dict_bplist(char *out, size_t cap, const char *const *entries, size_t n_pairs) {
    if (cap < 200)
        return 0;

    size_t pos = 0;

    /* Header */
    memcpy(out + pos, "bplist00", 8);
    pos += 8;

    /* Reserve offset for dict (object 0) */
    size_t dict_offset = pos;

    /* Dict header */
    if (n_pairs > 14)
        return 0; /* keep small */
    out[pos++] = (char)(0xD0 | (n_pairs & 0x0F));

    /* Refs: keys first (1..n), then values (n+1..2n). 1 byte each. */
    for (size_t i = 0; i < n_pairs; i++) {
        out[pos++] = (char)(1 + i); /* key object refs */
    }
    for (size_t i = 0; i < n_pairs; i++) {
        out[pos++] = (char)(1 + n_pairs + i); /* value object refs */
    }

    /* Now emit the key strings, then the value strings, recording offsets. */
    size_t offsets[64];
    offsets[0] = dict_offset;

    for (size_t i = 0; i < n_pairs; i++) {
        offsets[1 + i] = pos;
        const char *k = entries[i * 2];
        size_t klen = strlen(k);
        if (klen > 14)
            return 0;
        out[pos++] = (char)(0x50 | (klen & 0x0F));
        memcpy(out + pos, k, klen);
        pos += klen;
    }
    for (size_t i = 0; i < n_pairs; i++) {
        offsets[1 + n_pairs + i] = pos;
        const char *v = entries[i * 2 + 1];
        size_t vlen = strlen(v);
        if (vlen > 14) {
            /* Use long-form length */
            out[pos++] = 0x5F;
            out[pos++] = 0x10;
            out[pos++] = (char)vlen;
        } else {
            out[pos++] = (char)(0x50 | (vlen & 0x0F));
        }
        memcpy(out + pos, v, vlen);
        pos += vlen;
    }

    /* Offset table */
    size_t offset_table_offset = pos;
    size_t num_objects = 1 + 2 * n_pairs;
    for (size_t i = 0; i < num_objects; i++) {
        out[pos++] = (char)offsets[i];
    }

    /* Trailer: 6 unused + sort_version + offset_int_size + ref_size +
     * num_objects(8) + top_object(8) + offset_table_offset(8) */
    for (int i = 0; i < 6; i++)
        out[pos++] = 0;
    out[pos++] = 0; /* sort_version */
    out[pos++] = 1; /* offset_int_size */
    out[pos++] = 1; /* ref_size */
    /* num_objects 8 bytes big-endian */
    for (int i = 0; i < 7; i++)
        out[pos++] = 0;
    out[pos++] = (char)num_objects;
    /* top_object */
    for (int i = 0; i < 8; i++)
        out[pos++] = 0;
    /* offset_table_offset 8 bytes big-endian */
    for (int i = 0; i < 7; i++)
        out[pos++] = 0;
    out[pos++] = (char)offset_table_offset;

    return pos;
}

/* Privacy check: assert no digit run > 2 chars and no "$" / "USD" /
 * specific amount strings appear in `text`. */
static bool privacy_clean_money(const char *text) {
    if (!text)
        return true;
    if (strstr(text, "$") || strstr(text, "USD") || strstr(text, "EUR"))
        return false;
    size_t run = 0;
    for (size_t i = 0; text[i]; i++) {
        if (isdigit((unsigned char)text[i]))
            run++;
        else
            run = 0;
        if (run > 2)
            return false;
    }
    return true;
}

/* ── URL preview ──────────────────────────────────────────────────── */

static void test_decode_url_preview_uses_title(void) {
    char blob[512];
    const char *entries[] = {"title", "OpenAI announces new model", "originalURL",
                             "https://example.com"};
    size_t n = build_dict_bplist(blob, sizeof(blob), entries, 2);
    HU_ASSERT_TRUE(n > 0);

    char out[256];
    size_t r = hu_imessage_decode_url_preview((unsigned char *)blob, n, out, sizeof(out));
    HU_ASSERT_TRUE(r > 0);
    HU_ASSERT_TRUE(strstr(out, "OpenAI") != NULL);
}

static void test_decode_url_preview_falls_back_to_url(void) {
    char blob[512];
    /* No title/summary — only URL key. */
    const char *entries[] = {"originalURL", "https://example.com/long-path"};
    size_t n = build_dict_bplist(blob, sizeof(blob), entries, 1);
    HU_ASSERT_TRUE(n > 0);

    char out[256];
    size_t r = hu_imessage_decode_url_preview((unsigned char *)blob, n, out, sizeof(out));
    HU_ASSERT_TRUE(r > 0);
    HU_ASSERT_TRUE(strstr(out, "example.com") != NULL);
}

static void test_decode_url_preview_empty_dict_returns_zero(void) {
    char blob[512];
    const char *entries[] = {"unrelated_key", "value"};
    size_t n = build_dict_bplist(blob, sizeof(blob), entries, 1);
    char out[256];
    size_t r = hu_imessage_decode_url_preview((unsigned char *)blob, n, out, sizeof(out));
    HU_ASSERT_EQ((int)r, 0);
}

/* ── Apple Pay — privacy contracts ────────────────────────────────── */

static void test_decode_apple_pay_recipient_only_no_amount_leak(void) {
    char blob[512];
    /* Adversarial: payload contains BOTH recipient AND amount/currency.
     * Output must contain recipient and NOT amount/currency. */
    const char *entries[] = {"recipient", "+15551234567", "amount", "1500", "currency", "USD"};
    size_t n = build_dict_bplist(blob, sizeof(blob), entries, 3);
    HU_ASSERT_TRUE(n > 0);

    char out[256] = {0};
    size_t r = hu_imessage_decode_apple_pay((unsigned char *)blob, n, out, sizeof(out));
    HU_ASSERT_TRUE(r > 0);
    HU_ASSERT_TRUE(strstr(out, "+15551234567") != NULL);

    /* Privacy contract: never emit dollar/currency/amount. */
    HU_ASSERT_TRUE(strstr(out, "$") == NULL);
    HU_ASSERT_TRUE(strstr(out, "USD") == NULL);
    HU_ASSERT_TRUE(strstr(out, "1500") == NULL);
    /* Phone-number digits are allowed in the recipient handle, but we
     * still verify no STANDALONE money-looking string. Phone has 11
     * digits in a single run; we accept that as part of the handle.
     * The contract is "no NEW digit leak from amount keys" — encoded
     * structurally by the decoder never reading those keys. */
}

static void test_decode_apple_pay_only_amount_returns_zero(void) {
    /* No recipient = no leak. */
    char blob[512];
    const char *entries[] = {"amount", "9999", "currency", "USD"};
    size_t n = build_dict_bplist(blob, sizeof(blob), entries, 2);
    HU_ASSERT_TRUE(n > 0);

    char out[256] = {0};
    size_t r = hu_imessage_decode_apple_pay((unsigned char *)blob, n, out, sizeof(out));
    HU_ASSERT_EQ((int)r, 0);
    HU_ASSERT_TRUE(out[0] == '\0');
}

static void test_decode_apple_pay_handle_field_works(void) {
    /* Some payloads use `handle` instead of `recipient`. */
    char blob[512];
    const char *entries[] = {"handle", "alice@example.com"};
    size_t n = build_dict_bplist(blob, sizeof(blob), entries, 1);
    char out[256];
    size_t r = hu_imessage_decode_apple_pay((unsigned char *)blob, n, out, sizeof(out));
    HU_ASSERT_TRUE(r > 0);
    HU_ASSERT_TRUE(strstr(out, "alice@example.com") != NULL);
    HU_ASSERT_TRUE(privacy_clean_money(out));
}

/* ── Placemark — privacy contracts ────────────────────────────────── */

static void test_decode_placemark_uses_name(void) {
    char blob[512];
    const char *entries[] = {"name", "Tahoe City, CA"};
    size_t n = build_dict_bplist(blob, sizeof(blob), entries, 1);
    char out[256];
    size_t r = hu_imessage_decode_placemark((unsigned char *)blob, n, out, sizeof(out));
    HU_ASSERT_TRUE(r > 0);
    HU_ASSERT_TRUE(strstr(out, "Tahoe City") != NULL);
}

static void test_decode_placemark_coordinates_only_returns_zero(void) {
    /* Adversarial: payload has ONLY coordinates, no place name. */
    char blob[512];
    const char *entries[] = {"latitude", "37.7749", "longitude", "-122.4194"};
    size_t n = build_dict_bplist(blob, sizeof(blob), entries, 2);
    char out[256] = {0};
    size_t r = hu_imessage_decode_placemark((unsigned char *)blob, n, out, sizeof(out));
    HU_ASSERT_EQ((int)r, 0);
    /* And critically: no leak. */
    HU_ASSERT_TRUE(strstr(out, "37") == NULL);
    HU_ASSERT_TRUE(strstr(out, "122") == NULL);
}

/* ── Music ────────────────────────────────────────────────────────── */

static void test_decode_music_renders_song_by_artist(void) {
    char blob[512];
    const char *entries[] = {"songName", "Yesterday", "artistName", "The Beatles"};
    size_t n = build_dict_bplist(blob, sizeof(blob), entries, 2);
    char out[256];
    size_t r = hu_imessage_decode_music((unsigned char *)blob, n, out, sizeof(out));
    HU_ASSERT_TRUE(r > 0);
    HU_ASSERT_TRUE(strstr(out, "Yesterday") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Beatles") != NULL);
    HU_ASSERT_TRUE(strstr(out, " by ") != NULL);
}

/* ── Poll ─────────────────────────────────────────────────────────── */

static void test_decode_poll_extracts_question(void) {
    char blob[512];
    const char *entries[] = {"question", "What time should we leave?"};
    size_t n = build_dict_bplist(blob, sizeof(blob), entries, 1);
    char out[256];
    size_t r = hu_imessage_decode_poll((unsigned char *)blob, n, out, sizeof(out));
    HU_ASSERT_TRUE(r > 0);
    HU_ASSERT_TRUE(strstr(out, "What time") != NULL);
}

/* ── Dispatch ─────────────────────────────────────────────────────── */

static void test_balloon_decode_dispatches_by_bundle_id(void) {
    char blob[512];
    const char *entries[] = {"question", "Vote now"};
    size_t n = build_dict_bplist(blob, sizeof(blob), entries, 1);

    char out[256];
    hu_imessage_balloon_kind_t kind = hu_imessage_balloon_decode(
        "com.apple.messages.PollBalloon", (unsigned char *)blob, n, out, sizeof(out));
    HU_ASSERT_EQ((int)kind, (int)HU_IMESSAGE_BALLOON_POLL);
    HU_ASSERT_TRUE(strstr(out, "Vote now") != NULL);
}

static void test_balloon_decode_unknown_bundle_returns_unknown(void) {
    char out[256] = {0};
    hu_imessage_balloon_kind_t kind =
        hu_imessage_balloon_decode("com.acme.unrelated.thing", NULL, 0, out, sizeof(out));
    HU_ASSERT_EQ((int)kind, (int)HU_IMESSAGE_BALLOON_UNKNOWN);
    HU_ASSERT_TRUE(out[0] == '\0');
}

/* ── Malformed input safety ───────────────────────────────────────── */

static void test_decoders_handle_null_safely(void) {
    char out[256];
    HU_ASSERT_EQ((int)hu_imessage_decode_url_preview(NULL, 0, out, sizeof(out)), 0);
    HU_ASSERT_EQ((int)hu_imessage_decode_apple_pay(NULL, 0, out, sizeof(out)), 0);
    HU_ASSERT_EQ((int)hu_imessage_decode_placemark(NULL, 0, out, sizeof(out)), 0);
    HU_ASSERT_EQ((int)hu_imessage_decode_music(NULL, 0, out, sizeof(out)), 0);
    HU_ASSERT_EQ((int)hu_imessage_decode_poll(NULL, 0, out, sizeof(out)), 0);
}

static void test_decoders_handle_garbage_blob_safely(void) {
    unsigned char garbage[64] = {0};
    memcpy(garbage, "NOT_A_PLIST", 11);
    char out[256];
    /* Garbage input must not crash; returns 0. */
    HU_ASSERT_EQ((int)hu_imessage_decode_url_preview(garbage, sizeof(garbage), out, sizeof(out)),
                 0);
    HU_ASSERT_EQ((int)hu_imessage_decode_apple_pay(garbage, sizeof(garbage), out, sizeof(out)), 0);
}

void run_imessage_balloon_decode_tests(void) {
    HU_TEST_SUITE("imessage_balloon_decode");
    HU_RUN_TEST(test_decode_url_preview_uses_title);
    HU_RUN_TEST(test_decode_url_preview_falls_back_to_url);
    HU_RUN_TEST(test_decode_url_preview_empty_dict_returns_zero);
    HU_RUN_TEST(test_decode_apple_pay_recipient_only_no_amount_leak);
    HU_RUN_TEST(test_decode_apple_pay_only_amount_returns_zero);
    HU_RUN_TEST(test_decode_apple_pay_handle_field_works);
    HU_RUN_TEST(test_decode_placemark_uses_name);
    HU_RUN_TEST(test_decode_placemark_coordinates_only_returns_zero);
    HU_RUN_TEST(test_decode_music_renders_song_by_artist);
    HU_RUN_TEST(test_decode_poll_extracts_question);
    HU_RUN_TEST(test_balloon_decode_dispatches_by_bundle_id);
    HU_RUN_TEST(test_balloon_decode_unknown_bundle_returns_unknown);
    HU_RUN_TEST(test_decoders_handle_null_safely);
    HU_RUN_TEST(test_decoders_handle_garbage_blob_safely);
}

#else /* !HU_HAS_IMESSAGE — stub runner so the symbol resolves */

void run_imessage_balloon_decode_tests(void) {
    (void)0;
}

#endif /* HU_HAS_IMESSAGE */
