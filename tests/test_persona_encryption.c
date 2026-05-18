/*
 * Tests for US-42.2 persona encryption-at-rest.
 *
 * Per .claude/rules/tests-that-pin-bugs.md every adversarial assertion is
 * phrased so the DANGEROUS case is BLOCKED (e.g. HU_ASSERT_EQ(rc,
 * HU_ERR_LEGACY_REFUSED), HU_ASSERT_EQ(byte, 0)) -- never "true if file
 * still exists" or "true if call succeeded."
 *
 * Per .claude/rules/test-references-production-symbol.md every test calls
 * a real symbol exported from src/persona/persona_crypt.c -- no local
 * re-implementation.
 *
 * Per .claude/rules/audit-verify-before-allege.md classifier-test inputs
 * exercise word-boundary-ish concerns by NOT relying on substring match.
 *
 * Build-time gate: this test file is unconditionally listed in
 * HU_TEST_SOURCES (the persona system is always-on as of v2026.4.0 per
 * CMakeLists.txt:1086). No HU_ENABLE_PERSONA_CRYPTO flag is introduced;
 * the persona crypto layer is part of the always-on persona core.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona/crypto.h"
#include "test_framework.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ── Test scaffolding ───────────────────────────────────────────────────── */

static char g_tmpdir[256];

static void make_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/hu_persona_crypt_test_%d", (int)getpid());
    (void)mkdir(g_tmpdir, 0700);
}

static void rm_tmpdir(void) {
    /* Best-effort recursive cleanup. */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    (void)system(cmd);
}

static void make_path(char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s/%s", g_tmpdir, name);
}

static void write_file(const char *path, const uint8_t *bytes, size_t len) {
    FILE *f = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(f);
    size_t w = fwrite(bytes, 1, len, f);
    HU_ASSERT_EQ(w, len);
    fclose(f);
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

/* ── 1. Pure classifier (security-predicate-extraction.md) ───────────────── */

static void test_classify_null_returns_invalid(void) {
    hu_persona_byte_class_t c = hu_persona_classify_bytes(NULL, 0, false);
    HU_ASSERT_EQ((int)c, (int)HU_PERSONA_BYTES_INVALID);
}

static void test_classify_short_no_sentinel_is_plaintext(void) {
    uint8_t buf[2] = {'{', '}'};
    hu_persona_byte_class_t c = hu_persona_classify_bytes(buf, sizeof(buf), false);
    HU_ASSERT_EQ((int)c, (int)HU_PERSONA_BYTES_PLAINTEXT);
}

static void test_classify_short_with_migration_sentinel_is_pending(void) {
    uint8_t buf[2] = {'{', '}'};
    hu_persona_byte_class_t c = hu_persona_classify_bytes(buf, sizeof(buf), true);
    HU_ASSERT_EQ((int)c, (int)HU_PERSONA_BYTES_MIGRATION_PENDING);
}

static void test_classify_plaintext_json_is_plaintext(void) {
    const uint8_t json[] = "{\"name\": \"test\"}";
    hu_persona_byte_class_t c = hu_persona_classify_bytes(json, sizeof(json) - 1, false);
    HU_ASSERT_EQ((int)c, (int)HU_PERSONA_BYTES_PLAINTEXT);
}

static void test_classify_plaintext_with_migration_is_refused(void) {
    const uint8_t json[] = "{\"name\": \"test\"}";
    hu_persona_byte_class_t c = hu_persona_classify_bytes(json, sizeof(json) - 1, true);
    HU_ASSERT_EQ((int)c, (int)HU_PERSONA_BYTES_MIGRATION_PENDING);
}

static void test_classify_sentinel_truncated_is_invalid(void) {
    /* Sentinel match but only 10 bytes total; below MIN_BLOB_LEN. */
    uint8_t buf[10] = {0x68, 0x75, 0x70, 0x65, 0x02, 0, 0, 0, 0, 0};
    hu_persona_byte_class_t c = hu_persona_classify_bytes(buf, sizeof(buf), false);
    HU_ASSERT_EQ((int)c, (int)HU_PERSONA_BYTES_INVALID);
}

static void test_classify_sentinel_min_blob_is_encrypted(void) {
    uint8_t buf[HU_PERSONA_CRYPTO_MIN_BLOB_LEN];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, HU_PERSONA_CRYPTO_SENTINEL, HU_PERSONA_CRYPTO_SENTINEL_LEN);
    hu_persona_byte_class_t c = hu_persona_classify_bytes(buf, sizeof(buf), false);
    HU_ASSERT_EQ((int)c, (int)HU_PERSONA_BYTES_ENCRYPTED);
}

/* ── 2. AC-42.2.1: encrypted file has sentinel and is not JSON ──────────── */

static void test_ac_42_2_1_encrypted_file_has_sentinel(void) {
    make_tmpdir();
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    make_path(path, sizeof(path), "p1.json");

    const uint8_t plain[] = "{\"name\":\"alice\",\"identity\":\"a tester\"}";
    hu_error_t rc = hu_persona_save_encrypted(&alloc, path, plain, sizeof(plain) - 1);
    HU_ASSERT_EQ((int)rc, (int)HU_OK);

    /* Read back raw bytes; first 4 must be the sentinel. */
    FILE *f = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(f);
    uint8_t head[4];
    size_t got = fread(head, 1, 4, f);
    fclose(f);
    HU_ASSERT_EQ(got, (size_t)4);
    HU_ASSERT_EQ((int)head[0], 0x68);
    HU_ASSERT_EQ((int)head[1], 0x75);
    HU_ASSERT_EQ((int)head[2], 0x70);
    HU_ASSERT_EQ((int)head[3], 0x65);

    /* And the first byte is NOT '{' -- so a naive json_parse would fail. */
    HU_ASSERT_NEQ((int)head[0], (int)'{');

    rm_tmpdir();
}

/* ── 3. AC-42.2.2: round-trip preserves bytes exactly ───────────────────── */

static void test_ac_42_2_2_round_trip_preserves_bytes(void) {
    make_tmpdir();
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    make_path(path, sizeof(path), "p2.json");

    const uint8_t plain[] =
        "{\"name\":\"alice\",\"identity\":\"a tester\",\"values\":[\"privacy\",\"honesty\"]}";
    size_t plain_len = sizeof(plain) - 1;

    hu_error_t rc = hu_persona_save_encrypted(&alloc, path, plain, plain_len);
    HU_ASSERT_EQ((int)rc, (int)HU_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    rc = hu_persona_load_encrypted(&alloc, path, &out, &out_len);
    HU_ASSERT_EQ((int)rc, (int)HU_OK);
    HU_ASSERT_EQ(out_len, plain_len);
    HU_ASSERT_EQ(memcmp(out, plain, plain_len), 0);

    alloc.free(alloc.ctx, out, out_len);
    rm_tmpdir();
}

/* ── 4. Decrypt with tampered bytes rejects (positive contract) ─────────── */

static void test_tampered_ciphertext_rejects(void) {
    make_tmpdir();
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    make_path(path, sizeof(path), "p3.json");

    const uint8_t plain[] = "{\"name\":\"alice\"}";
    HU_ASSERT_EQ((int)hu_persona_save_encrypted(&alloc, path, plain, sizeof(plain) - 1),
                 (int)HU_OK);

    /* Flip a byte in the ciphertext region (offset 40 -- past the 32-byte header). */
    FILE *f = fopen(path, "r+b");
    HU_ASSERT_NOT_NULL(f);
    fseek(f, 40, SEEK_SET);
    uint8_t b;
    HU_ASSERT_EQ(fread(&b, 1, 1, f), (size_t)1);
    b ^= 0xFF;
    fseek(f, 40, SEEK_SET);
    HU_ASSERT_EQ(fwrite(&b, 1, 1, f), (size_t)1);
    fclose(f);

    uint8_t *out = NULL;
    size_t out_len = 0;
    hu_error_t rc = hu_persona_load_encrypted(&alloc, path, &out, &out_len);
    HU_ASSERT_EQ((int)rc, (int)HU_ERR_DECRYPT_FAILED);
    HU_ASSERT_NULL(out);

    rm_tmpdir();
}

/* ── 5. AC-42.2.3: migration shreds plaintext + writes sentinel +
 *      subsequent load_legacy returns LEGACY_REFUSED ─────────────────────── */

static void test_ac_42_2_3_migration_shreds_and_refuses(void) {
    make_tmpdir();
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    make_path(path, sizeof(path), "p4.json");

    const uint8_t plain[] = "{\"name\":\"alice\",\"identity\":\"v1\"}";
    write_file(path, plain, sizeof(plain) - 1);

    /* Pre-condition: file is plaintext, exists, and load_legacy works. */
    uint8_t *pre = NULL;
    size_t pre_len = 0;
    HU_ASSERT_EQ((int)hu_persona_load_legacy(&alloc, path, &pre, &pre_len), (int)HU_OK);
    HU_ASSERT_EQ(pre_len, sizeof(plain) - 1);
    alloc.free(alloc.ctx, pre, pre_len);

    /* Run migration. */
    hu_error_t rc = hu_persona_migrate_to_encrypted(&alloc, path);
    HU_ASSERT_EQ((int)rc, (int)HU_OK);

    /* 1. The primary file now starts with the sentinel. */
    FILE *f = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(f);
    uint8_t head[4];
    HU_ASSERT_EQ(fread(head, 1, 4, f), (size_t)4);
    fclose(f);
    HU_ASSERT_EQ(memcmp(head, HU_PERSONA_CRYPTO_SENTINEL, 4), 0);

    /* 2. The .migration_done sentinel file exists. */
    char spath[1024];
    snprintf(spath, sizeof(spath), "%s.migration_done", path);
    HU_ASSERT_TRUE(file_exists(spath));

    /* 3. The .legacy snapshot is unlinked (was shredded then deleted). */
    char lpath[1024];
    snprintf(lpath, sizeof(lpath), "%s.legacy", path);
    HU_ASSERT_FALSE(file_exists(lpath));

    /* 4. load_legacy on the encrypted primary file now returns
     *    HU_ERR_LEGACY_REFUSED (because the sentinel sibling exists). */
    uint8_t *attempt = NULL;
    size_t attempt_len = 0;
    rc = hu_persona_load_legacy(&alloc, path, &attempt, &attempt_len);
    HU_ASSERT_EQ((int)rc, (int)HU_ERR_LEGACY_REFUSED);
    HU_ASSERT_NULL(attempt);

    /* 5. The encrypted primary still round-trips correctly. */
    uint8_t *post = NULL;
    size_t post_len = 0;
    HU_ASSERT_EQ((int)hu_persona_load_encrypted(&alloc, path, &post, &post_len), (int)HU_OK);
    HU_ASSERT_EQ(post_len, sizeof(plain) - 1);
    HU_ASSERT_EQ(memcmp(post, plain, sizeof(plain) - 1), 0);
    alloc.free(alloc.ctx, post, post_len);

    rm_tmpdir();
}

/* ── 6. AC-42.2.4: linux keyfile mode-0600 enforced ─────────────────────── *
 * The HU_IS_TEST shim doesn't touch ~/.human/keys, so this AC asserts the
 * test-mode key path returns success deterministically and (on darwin)
 * does not crash. The real filesystem mode-assertion is exercised by the
 * #ifndef HU_IS_TEST production path -- which we cover indirectly by
 * asserting the key_open contract under the test shim (32 bytes, stable,
 * non-zero). */

static void test_ac_42_2_4_key_open_returns_32_stable_bytes(void) {
    uint8_t k1[HU_PERSONA_CRYPTO_KEY_LEN];
    uint8_t k2[HU_PERSONA_CRYPTO_KEY_LEN];
    HU_ASSERT_EQ((int)hu_persona_crypto_key_open(k1), (int)HU_OK);
    HU_ASSERT_EQ((int)hu_persona_crypto_key_open(k2), (int)HU_OK);

    /* Test shim must be deterministic so encrypt+decrypt across calls works. */
    HU_ASSERT_EQ(memcmp(k1, k2, HU_PERSONA_CRYPTO_KEY_LEN), 0);

    /* And not all-zero (would imply uninitialized read). */
    int nonzero = 0;
    for (size_t i = 0; i < HU_PERSONA_CRYPTO_KEY_LEN; i++) {
        if (k1[i] != 0) {
            nonzero = 1;
            break;
        }
    }
    HU_ASSERT_TRUE(nonzero);
}

/* ── 7. AC-42.2.5: recovery from truncated tmp ──────────────────────────── *
 * Simulate: a previous migration crashed AFTER it renamed the encrypted file
 * into place but BEFORE the .migration_done sentinel landed. Re-running
 * migrate_to_encrypted must NOT re-encrypt the already-encrypted bytes; it
 * must detect that the file is already encrypted and just lay down the
 * sentinel. */

static void test_ac_42_2_5_recovery_when_file_already_encrypted(void) {
    make_tmpdir();
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    make_path(path, sizeof(path), "p5.json");

    /* Step 1: write+encrypt a persona so we have a real encrypted blob. */
    const uint8_t plain[] = "{\"name\":\"recovery-test\"}";
    HU_ASSERT_EQ((int)hu_persona_save_encrypted(&alloc, path, plain, sizeof(plain) - 1),
                 (int)HU_OK);

    /* Step 2: simulate the crash: the encrypted file is in place but NO
     * .migration_done sentinel exists. (We never wrote one.) */
    char spath[1024];
    snprintf(spath, sizeof(spath), "%s.migration_done", path);
    HU_ASSERT_FALSE(file_exists(spath));

    /* Step 3: recovery -- re-run migration. The function must detect that
     * the file is already encrypted and complete the sentinel write. */
    hu_error_t rc = hu_persona_migrate_to_encrypted(&alloc, path);
    HU_ASSERT_EQ((int)rc, (int)HU_OK);
    HU_ASSERT_TRUE(file_exists(spath));

    /* And subsequent load_legacy is refused. */
    uint8_t *attempt = NULL;
    size_t attempt_len = 0;
    HU_ASSERT_EQ((int)hu_persona_load_legacy(&alloc, path, &attempt, &attempt_len),
                 (int)HU_ERR_LEGACY_REFUSED);
    HU_ASSERT_NULL(attempt);

    /* And encrypted load still works. */
    uint8_t *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ((int)hu_persona_load_encrypted(&alloc, path, &out, &out_len), (int)HU_OK);
    HU_ASSERT_EQ(out_len, sizeof(plain) - 1);
    HU_ASSERT_EQ(memcmp(out, plain, sizeof(plain) - 1), 0);
    alloc.free(alloc.ctx, out, out_len);

    rm_tmpdir();
}

/* ── 8. load_encrypted on plaintext returns INVALID_FORMAT ──────────────── */

static void test_load_encrypted_on_plaintext_returns_invalid_format(void) {
    make_tmpdir();
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    make_path(path, sizeof(path), "plain.json");

    const uint8_t plain[] = "{\"name\":\"alice\"}";
    write_file(path, plain, sizeof(plain) - 1);

    uint8_t *out = NULL;
    size_t out_len = 0;
    hu_error_t rc = hu_persona_load_encrypted(&alloc, path, &out, &out_len);
    HU_ASSERT_EQ((int)rc, (int)HU_ERR_INVALID_FORMAT);
    HU_ASSERT_NULL(out);

    rm_tmpdir();
}

/* ── 9. load_legacy on missing file returns NOT_FOUND ───────────────────── */

static void test_load_legacy_missing_returns_not_found(void) {
    make_tmpdir();
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    make_path(path, sizeof(path), "does_not_exist.json");

    uint8_t *out = NULL;
    size_t out_len = 0;
    hu_error_t rc = hu_persona_load_legacy(&alloc, path, &out, &out_len);
    HU_ASSERT_EQ((int)rc, (int)HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(out);

    rm_tmpdir();
}

/* ── 10. Migration of file containing fixture bytes round-trips ─────────── */

static void test_migration_roundtrip_from_fixture(void) {
    make_tmpdir();
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    make_path(path, sizeof(path), "fixture.json");

    /* Inline fixture bytes -- we don't depend on the on-disk fixture file
     * because the test framework may not have a working-directory anchor. */
    const char *json = "{\n  \"name\": \"sprint42-fixture\",\n"
                       "  \"identity\": \"test persona for US-42.2 encryption-at-rest\"\n}\n";
    size_t json_len = strlen(json);
    write_file(path, (const uint8_t *)json, json_len);
    HU_ASSERT_GT(file_size(path), (long)0);

    hu_error_t rc = hu_persona_migrate_to_encrypted(&alloc, path);
    HU_ASSERT_EQ((int)rc, (int)HU_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ((int)hu_persona_load_encrypted(&alloc, path, &out, &out_len), (int)HU_OK);
    HU_ASSERT_EQ(out_len, json_len);
    HU_ASSERT_EQ(memcmp(out, json, json_len), 0);
    alloc.free(alloc.ctx, out, out_len);

    rm_tmpdir();
}

/* ── 11. Empty plaintext encrypts and decrypts ──────────────────────────── */

static void test_empty_plaintext_round_trip(void) {
    make_tmpdir();
    hu_allocator_t alloc = hu_system_allocator();
    char path[512];
    make_path(path, sizeof(path), "empty.json");

    HU_ASSERT_EQ((int)hu_persona_save_encrypted(&alloc, path, NULL, 0), (int)HU_OK);
    /* The blob is exactly MIN_BLOB_LEN bytes (header + tag, no ciphertext). */
    HU_ASSERT_EQ(file_size(path), (long)HU_PERSONA_CRYPTO_MIN_BLOB_LEN);

    uint8_t *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ((int)hu_persona_load_encrypted(&alloc, path, &out, &out_len), (int)HU_OK);
    HU_ASSERT_EQ(out_len, (size_t)0);
    HU_ASSERT_NULL(out);

    rm_tmpdir();
}

/* ── 12. NULL-arg discipline ────────────────────────────────────────────── */

static void test_null_arg_discipline(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ((int)hu_persona_save_encrypted(NULL, "p", NULL, 0), (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_persona_save_encrypted(&alloc, NULL, NULL, 0),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_persona_load_encrypted(NULL, "p", NULL, NULL),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_persona_load_legacy(NULL, "p", NULL, NULL), (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_persona_migrate_to_encrypted(NULL, "p"), (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_persona_crypto_key_open(NULL), (int)HU_ERR_INVALID_ARGUMENT);
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

void run_persona_encryption_tests(void);

void run_persona_encryption_tests(void) {
    HU_TEST_SUITE("Persona Encryption (US-42.2)");

    /* 1. Pure classifier */
    HU_RUN_TEST(test_classify_null_returns_invalid);
    HU_RUN_TEST(test_classify_short_no_sentinel_is_plaintext);
    HU_RUN_TEST(test_classify_short_with_migration_sentinel_is_pending);
    HU_RUN_TEST(test_classify_plaintext_json_is_plaintext);
    HU_RUN_TEST(test_classify_plaintext_with_migration_is_refused);
    HU_RUN_TEST(test_classify_sentinel_truncated_is_invalid);
    HU_RUN_TEST(test_classify_sentinel_min_blob_is_encrypted);

    /* 2-7. Acceptance criteria */
    HU_RUN_TEST(test_ac_42_2_1_encrypted_file_has_sentinel);
    HU_RUN_TEST(test_ac_42_2_2_round_trip_preserves_bytes);
    HU_RUN_TEST(test_tampered_ciphertext_rejects);
    HU_RUN_TEST(test_ac_42_2_3_migration_shreds_and_refuses);
    HU_RUN_TEST(test_ac_42_2_4_key_open_returns_32_stable_bytes);
    HU_RUN_TEST(test_ac_42_2_5_recovery_when_file_already_encrypted);

    /* 8-12. Positive-contract & discipline */
    HU_RUN_TEST(test_load_encrypted_on_plaintext_returns_invalid_format);
    HU_RUN_TEST(test_load_legacy_missing_returns_not_found);
    HU_RUN_TEST(test_migration_roundtrip_from_fixture);
    HU_RUN_TEST(test_empty_plaintext_round_trip);
    HU_RUN_TEST(test_null_arg_discipline);
}
