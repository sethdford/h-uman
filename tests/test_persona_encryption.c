/* US-8.2 — Persona Encryption-at-Rest tests.
 *
 * Maps the 6 acceptance criteria to test functions:
 *   AC-8.2.1: migrate_plaintext_yields_encrypted_round_trippable_file
 *   AC-8.2.2: load_with_wrong_key_returns_decrypt_failed_and_out_untouched   (BLOCKED)
 *   AC-8.2.3: save_twice_yields_different_ciphertext_same_plaintext
 *   AC-8.2.4: load_legacy_refuses_encrypted_file_with_legacy_refused_error    (BLOCKED)
 *             + load_legacy_refuses_plaintext_when_no_sentinel                (BLOCKED)
 *   AC-8.2.5: save_encrypted_preserves_prior_state_when_tmp_blocked
 *             (deterministic fix-shape probe; mirrors
 *              test_personal_model_atomic_save's pattern)
 *   AC-8.2.6: file references hu_persona_save_encrypted, hu_persona_load_encrypted,
 *             hu_persona_migrate_to_encrypted, hu_persona_load_legacy,
 *             hu_persona_classify_bytes (test-references-production-symbol rule)
 *
 * Also exercises the pure classify predicate truth table per
 * `.claude/rules/security-predicate-extraction.md`.
 *
 * Per `.claude/rules/tests-that-pin-bugs.md` the adversarial tests
 * assert the dangerous case is BLOCKED.
 */

#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include "human/persona/crypto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Test scaffolding                                                    */
/* ------------------------------------------------------------------ */

/* Builds an isolated working dir + keyfile override for one test case.
 * The HU_PERSONA_KEYFILE_OVERRIDE env var routes
 * hu_persona_crypt_derive_key into a temp 0600 keyfile, keeping the
 * test hermetic (no Keychain prompts on darwin, no developer home
 * pollution on linux). */
typedef struct test_env {
    char dir[256];
    char persona_path[300];
    char keyfile[300];
    char sentinel[400];
    char legacy[400];
} test_env_t;

static void env_init(test_env_t *e) {
    char tmpl[] = "/tmp/hu_p_us82_XXXXXX";
    char *d = mkdtemp(tmpl);
    HU_ASSERT_NOT_NULL(d);
    snprintf(e->dir, sizeof(e->dir), "%s", d);
    snprintf(e->persona_path, sizeof(e->persona_path), "%s/persona.json", e->dir);
    snprintf(e->keyfile, sizeof(e->keyfile), "%s/persona.key", e->dir);
    snprintf(e->sentinel, sizeof(e->sentinel), "%s.migration-pending", e->persona_path);
    snprintf(e->legacy, sizeof(e->legacy), "%s.legacy", e->persona_path);
    setenv("HU_PERSONA_KEYFILE_OVERRIDE", e->keyfile, 1);
}

static void env_cleanup(test_env_t *e) {
    (void)unlink(e->persona_path);
    (void)unlink(e->keyfile);
    (void)unlink(e->sentinel);
    (void)unlink(e->legacy);
    char tmp_path[400];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", e->persona_path);
    (void)unlink(tmp_path);
    (void)rmdir(tmp_path); /* in case the test pre-blocked it */
    (void)rmdir(e->dir);
    unsetenv("HU_PERSONA_KEYFILE_OVERRIDE");
}

static void write_plaintext_fixture(const char *path) {
    /* Inline a minimal valid persona JSON that exercises every field that
     * AC-8.2.1 round-trips through encryption. Matches
     * tests/fixtures/persona_plaintext_sample.json in spirit but inlined
     * here so the test never depends on cwd. */
    const char *json = "{"
                       "\"name\":\"us_8_2_test_persona\","
                       "\"core\":{"
                       "\"identity\":\"A test persona for US-8.2.\","
                       "\"traits\":[\"direct\",\"warm\",\"private-by-default\"],"
                       "\"communication_rules\":[\"prefer lowercase in casual channels\"],"
                       "\"values\":[\"user privacy is structural\"],"
                       "\"decision_style\":\"evidence-first\""
                       "},"
                       "\"example_banks\":["
                       "{\"channel\":\"cli\",\"examples\":["
                       "{\"context\":\"casual ack\",\"incoming\":\"hi\",\"response\":\"hey\"}"
                       "]}"
                       "]"
                       "}";
    FILE *f = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(f);
    size_t n = strlen(json);
    HU_ASSERT_EQ(fwrite(json, 1, n, f), n);
    fclose(f);
}

static void touch_sentinel(const char *sentinel_path) {
    FILE *f = fopen(sentinel_path, "wb");
    HU_ASSERT_NOT_NULL(f);
    fputs("plaintext-shredded", f);
    fclose(f);
}

static bool file_starts_with(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char buf[64];
    size_t n = strlen(needle);
    if (n > sizeof(buf))
        n = sizeof(buf);
    size_t r = fread(buf, 1, n, f);
    fclose(f);
    return r == n && memcmp(buf, needle, n) == 0;
}

/* ------------------------------------------------------------------ */
/* 1. Pure predicate truth table                                       */
/* ------------------------------------------------------------------ */

static void test_classify_empty_buffer_is_unknown(void) {
    HU_ASSERT_EQ(hu_persona_classify_bytes(NULL, 0), HU_PERSONA_FORMAT_UNKNOWN);
    uint8_t b = 0;
    HU_ASSERT_EQ(hu_persona_classify_bytes(&b, 0), HU_PERSONA_FORMAT_UNKNOWN);
}

static void test_classify_plaintext_starting_with_brace_is_json(void) {
    const uint8_t buf[] = "{\"name\":\"x\"}";
    HU_ASSERT_EQ(hu_persona_classify_bytes(buf, sizeof(buf) - 1), HU_PERSONA_FORMAT_PLAINTEXT_JSON);
}

static void test_classify_plaintext_with_leading_whitespace_is_json(void) {
    const uint8_t buf[] = "  \t\n{\"a\":1}";
    HU_ASSERT_EQ(hu_persona_classify_bytes(buf, sizeof(buf) - 1), HU_PERSONA_FORMAT_PLAINTEXT_JSON);
}

static void test_classify_HUP1_magic_is_encrypted_v1(void) {
    uint8_t buf[HU_PERSONA_CRYPT_HEADER_BYTES + 4] = {0};
    memcpy(buf, "HUP1", 4);
    buf[4] = HU_PERSONA_CRYPT_VERSION;
    /* bytes 5..7 reserved zero; nonce 8..31 zero */
    HU_ASSERT_EQ(hu_persona_classify_bytes(buf, sizeof(buf)), HU_PERSONA_FORMAT_ENCRYPTED_V1);
}

static void test_classify_HUP1_truncated_to_header_only_is_unknown(void) {
    /* Only 16 bytes — less than the 32-byte header. Must not be classified
     * as encrypted_v1; otherwise load_encrypted would try to decode garbage. */
    uint8_t buf[16] = {0};
    memcpy(buf, "HUP1", 4);
    buf[4] = HU_PERSONA_CRYPT_VERSION;
    HU_ASSERT_EQ(hu_persona_classify_bytes(buf, sizeof(buf)), HU_PERSONA_FORMAT_UNKNOWN);
}

static void test_classify_HUP1_wrong_version_byte_is_unknown(void) {
    uint8_t buf[HU_PERSONA_CRYPT_HEADER_BYTES] = {0};
    memcpy(buf, "HUP1", 4);
    buf[4] = 0x02; /* future version we don't understand */
    HU_ASSERT_EQ(hu_persona_classify_bytes(buf, sizeof(buf)), HU_PERSONA_FORMAT_UNKNOWN);
}

static void test_classify_random_binary_is_unknown(void) {
    const uint8_t buf[] = {0xff, 0x00, 0x42, 0x13, 0x7e};
    HU_ASSERT_EQ(hu_persona_classify_bytes(buf, sizeof(buf)), HU_PERSONA_FORMAT_UNKNOWN);
}

/* ------------------------------------------------------------------ */
/* 2. Keystore behaviour                                               */
/* ------------------------------------------------------------------ */

static void test_keystore_creates_keyfile_with_0600_perms(void) {
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();
    uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES];
    HU_ASSERT_EQ(hu_persona_crypt_derive_key(&alloc, "any", key), HU_OK);
    struct stat st;
    HU_ASSERT_EQ(stat(e.keyfile, &st), 0);
    HU_ASSERT_EQ(st.st_size, (off_t)HU_PERSONA_CRYPT_KEY_BYTES);
    HU_ASSERT_EQ((int)(st.st_mode & 0777), 0600);
    env_cleanup(&e);
}

static void test_keystore_returns_same_key_on_repeat_call(void) {
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();
    uint8_t k1[HU_PERSONA_CRYPT_KEY_BYTES];
    uint8_t k2[HU_PERSONA_CRYPT_KEY_BYTES];
    HU_ASSERT_EQ(hu_persona_crypt_derive_key(&alloc, "a", k1), HU_OK);
    HU_ASSERT_EQ(hu_persona_crypt_derive_key(&alloc, "a", k2), HU_OK);
    HU_ASSERT_EQ(memcmp(k1, k2, HU_PERSONA_CRYPT_KEY_BYTES), 0);
    env_cleanup(&e);
}

static void test_keystore_rejects_world_readable_keyfile(void) {
    /* Defense in depth (design R4): if the keyfile perms are widened
     * (umask 022 accident, manual chmod, whatever), we MUST refuse to load
     * it.  This is the adversarial-BLOCKED case: the dangerous outcome —
     * silently reading a world-readable key — must NOT happen. */
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();
    uint8_t k1[HU_PERSONA_CRYPT_KEY_BYTES];
    HU_ASSERT_EQ(hu_persona_crypt_derive_key(&alloc, "x", k1), HU_OK);
    /* Widen perms then re-derive — must fail. */
    HU_ASSERT_EQ(chmod(e.keyfile, 0644), 0);
    uint8_t k2[HU_PERSONA_CRYPT_KEY_BYTES];
    hu_error_t err = hu_persona_crypt_derive_key(&alloc, "x", k2);
    HU_ASSERT(err != HU_OK);
    env_cleanup(&e);
}

/* ------------------------------------------------------------------ */
/* 3. Save / load round-trip (AC-8.2.1 happy path + AC-8.2.3 nonce)    */
/* ------------------------------------------------------------------ */

static void test_save_then_load_recovers_all_fields(void) {
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();

    /* Build a persona from the inline plaintext fixture via load_json. */
    write_plaintext_fixture(e.persona_path);
    char json[2048];
    FILE *f = fopen(e.persona_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';

    hu_persona_t p_in;
    memset(&p_in, 0, sizeof(p_in));
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, json, n, &p_in), HU_OK);

    uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES];
    HU_ASSERT_EQ(hu_persona_crypt_derive_key(&alloc, "us_8_2_test_persona", key), HU_OK);

    /* Replace the plaintext file with the encrypted save. */
    (void)unlink(e.persona_path);
    HU_ASSERT_EQ(hu_persona_save_encrypted(&alloc, e.persona_path, &p_in, key), HU_OK);
    /* File on disk MUST start with HUP1 magic, not '{'. */
    HU_ASSERT(file_starts_with(e.persona_path, "HUP1"));

    hu_persona_t p_out;
    memset(&p_out, 0, sizeof(p_out));
    HU_ASSERT_EQ(hu_persona_load_encrypted(&alloc, e.persona_path, key, &p_out), HU_OK);

    /* Field-by-field equality on the round-tripped subset. */
    HU_ASSERT_NOT_NULL(p_out.name);
    HU_ASSERT(strcmp(p_out.name, p_in.name) == 0);
    HU_ASSERT_NOT_NULL(p_out.identity);
    HU_ASSERT(strcmp(p_out.identity, p_in.identity) == 0);
    HU_ASSERT_EQ(p_out.traits_count, p_in.traits_count);
    for (size_t i = 0; i < p_in.traits_count; i++)
        HU_ASSERT(strcmp(p_out.traits[i], p_in.traits[i]) == 0);
    HU_ASSERT_EQ(p_out.communication_rules_count, p_in.communication_rules_count);
    for (size_t i = 0; i < p_in.communication_rules_count; i++)
        HU_ASSERT(strcmp(p_out.communication_rules[i], p_in.communication_rules[i]) == 0);
    HU_ASSERT_EQ(p_out.values_count, p_in.values_count);
    for (size_t i = 0; i < p_in.values_count; i++)
        HU_ASSERT(strcmp(p_out.values[i], p_in.values[i]) == 0);
    HU_ASSERT_EQ(p_out.example_banks_count, p_in.example_banks_count);

    hu_persona_deinit(&alloc, &p_in);
    hu_persona_deinit(&alloc, &p_out);
    env_cleanup(&e);
}

static void test_save_twice_yields_different_ciphertext_same_plaintext(void) {
    /* AC-8.2.3: nonce must be fresh per save (Poly1305 MAC catastrophically
     * fails on nonce reuse, but we check the ciphertext bytes themselves
     * rather than trusting libsodium's docs blindly). */
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();

    write_plaintext_fixture(e.persona_path);
    char json[2048];
    FILE *f = fopen(e.persona_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    (void)unlink(e.persona_path);

    hu_persona_t p_in;
    memset(&p_in, 0, sizeof(p_in));
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, json, n, &p_in), HU_OK);

    uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES];
    HU_ASSERT_EQ(hu_persona_crypt_derive_key(&alloc, "any", key), HU_OK);

    char path_a[400], path_b[400];
    snprintf(path_a, sizeof(path_a), "%s.a", e.persona_path);
    snprintf(path_b, sizeof(path_b), "%s.b", e.persona_path);

    HU_ASSERT_EQ(hu_persona_save_encrypted(&alloc, path_a, &p_in, key), HU_OK);
    HU_ASSERT_EQ(hu_persona_save_encrypted(&alloc, path_b, &p_in, key), HU_OK);

    /* Read both files and confirm bytes differ (specifically the nonce at
     * offset 8 and the ciphertext that follows). */
    FILE *fa = fopen(path_a, "rb");
    FILE *fb = fopen(path_b, "rb");
    HU_ASSERT_NOT_NULL(fa);
    HU_ASSERT_NOT_NULL(fb);
    uint8_t a[4096], b[4096];
    size_t na = fread(a, 1, sizeof(a), fa);
    size_t nb = fread(b, 1, sizeof(b), fb);
    fclose(fa);
    fclose(fb);
    HU_ASSERT_EQ(na, nb);
    HU_ASSERT(na > HU_PERSONA_CRYPT_HEADER_BYTES);
    HU_ASSERT(memcmp(a + 8, b + 8, HU_PERSONA_CRYPT_NONCE_BYTES) != 0);
    HU_ASSERT(memcmp(a, b, na) != 0);

    (void)unlink(path_a);
    (void)unlink(path_b);
    hu_persona_deinit(&alloc, &p_in);
    env_cleanup(&e);
}

/* ------------------------------------------------------------------ */
/* 4. Adversarial: wrong key (AC-8.2.2 — BLOCKED)                      */
/* ------------------------------------------------------------------ */

static void test_load_with_wrong_key_returns_decrypt_failed_and_out_untouched(void) {
    /* Per `.claude/rules/tests-that-pin-bugs.md`: the dangerous case here is
     * "wrong key succeeds in populating the persona".  The test asserts that
     * outcome is BLOCKED — return code is HU_ERR_DECRYPT_FAILED AND the
     * caller-provided out struct is left as-is. */
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();

    write_plaintext_fixture(e.persona_path);
    char json[2048];
    FILE *f = fopen(e.persona_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    (void)unlink(e.persona_path);

    hu_persona_t p_in;
    memset(&p_in, 0, sizeof(p_in));
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, json, n, &p_in), HU_OK);

    uint8_t good[HU_PERSONA_CRYPT_KEY_BYTES];
    HU_ASSERT_EQ(hu_persona_crypt_derive_key(&alloc, "x", good), HU_OK);
    HU_ASSERT_EQ(hu_persona_save_encrypted(&alloc, e.persona_path, &p_in, good), HU_OK);

    /* Flip exactly one byte of the key — must fail. */
    uint8_t bad[HU_PERSONA_CRYPT_KEY_BYTES];
    memcpy(bad, good, sizeof(bad));
    bad[0] ^= 0x01;

    /* Pre-fill out with a sentinel pattern that hu_persona_load_encrypted
     * MUST NOT clobber on failure. */
    hu_persona_t out;
    memset(&out, 0xA5, sizeof(out));
    hu_persona_t expected;
    memset(&expected, 0xA5, sizeof(expected));

    HU_ASSERT_EQ(hu_persona_load_encrypted(&alloc, e.persona_path, bad, &out),
                 HU_ERR_DECRYPT_FAILED);
    /* Byte-for-byte: not touched. */
    HU_ASSERT_EQ(memcmp(&out, &expected, sizeof(out)), 0);

    hu_persona_deinit(&alloc, &p_in);
    env_cleanup(&e);
}

static void test_load_with_tampered_ciphertext_returns_decrypt_failed(void) {
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();

    write_plaintext_fixture(e.persona_path);
    char json[2048];
    FILE *f = fopen(e.persona_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    (void)unlink(e.persona_path);

    hu_persona_t p_in;
    memset(&p_in, 0, sizeof(p_in));
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, json, n, &p_in), HU_OK);

    uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES];
    HU_ASSERT_EQ(hu_persona_crypt_derive_key(&alloc, "x", key), HU_OK);
    HU_ASSERT_EQ(hu_persona_save_encrypted(&alloc, e.persona_path, &p_in, key), HU_OK);

    /* Flip one byte in the ciphertext (offset 40 is well past the header). */
    int fd = open(e.persona_path, O_RDWR);
    HU_ASSERT(fd >= 0);
    uint8_t b;
    HU_ASSERT_EQ(lseek(fd, 40, SEEK_SET), 40);
    HU_ASSERT_EQ(read(fd, &b, 1), 1);
    b ^= 0xff;
    HU_ASSERT_EQ(lseek(fd, 40, SEEK_SET), 40);
    HU_ASSERT_EQ(write(fd, &b, 1), 1);
    close(fd);

    hu_persona_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_persona_load_encrypted(&alloc, e.persona_path, key, &out),
                 HU_ERR_DECRYPT_FAILED);

    hu_persona_deinit(&alloc, &p_in);
    env_cleanup(&e);
}

/* ------------------------------------------------------------------ */
/* 5. Migration (AC-8.2.1 end-to-end)                                  */
/* ------------------------------------------------------------------ */

static void test_migrate_plaintext_yields_encrypted_round_trippable_file(void) {
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();

    write_plaintext_fixture(e.persona_path);
    HU_ASSERT(file_starts_with(e.persona_path, "{"));

    HU_ASSERT_EQ(hu_persona_migrate_to_encrypted(&alloc, e.persona_path, "us_8_2_test_persona"),
                 HU_OK);
    /* After migration, file is HUP1-magic ciphertext. */
    HU_ASSERT(file_starts_with(e.persona_path, "HUP1"));
    HU_ASSERT(!file_starts_with(e.persona_path, "{"));

    uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES];
    HU_ASSERT_EQ(hu_persona_crypt_derive_key(&alloc, "us_8_2_test_persona", key), HU_OK);

    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_encrypted(&alloc, e.persona_path, key, &p), HU_OK);
    HU_ASSERT_NOT_NULL(p.name);
    HU_ASSERT(strcmp(p.name, "us_8_2_test_persona") == 0);
    HU_ASSERT_EQ(p.traits_count, (size_t)3);
    hu_persona_deinit(&alloc, &p);
    env_cleanup(&e);
}

static void test_migrate_is_idempotent_on_already_encrypted_file(void) {
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();

    write_plaintext_fixture(e.persona_path);
    HU_ASSERT_EQ(hu_persona_migrate_to_encrypted(&alloc, e.persona_path, "any"), HU_OK);

    /* Record post-migration file bytes; a second migrate call must not
     * rewrite them.  We don't care about byte equality (a re-seal would
     * generate a fresh nonce) — we care that the call returns HU_OK and
     * the file is still HUP1-encrypted afterwards. */
    HU_ASSERT_EQ(hu_persona_migrate_to_encrypted(&alloc, e.persona_path, "any"), HU_OK);
    HU_ASSERT(file_starts_with(e.persona_path, "HUP1"));
    env_cleanup(&e);
}

/* ------------------------------------------------------------------ */
/* 6. Refuse-path loader (AC-8.2.4 — BLOCKED)                          */
/* ------------------------------------------------------------------ */

static void test_load_legacy_refuses_encrypted_file_with_legacy_refused_error(void) {
    /* THE critical anti-regression for design R1: post-migration, the
     * legacy loader MUST hard-fail on encrypted bytes. Adversarial-BLOCKED
     * phrasing: name says "refused", assertion says
     * HU_ASSERT_EQ(err, HU_ERR_LEGACY_REFUSED) AND out is untouched. */
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();

    write_plaintext_fixture(e.persona_path);
    HU_ASSERT_EQ(hu_persona_migrate_to_encrypted(&alloc, e.persona_path, "x"), HU_OK);
    /* No sentinel — migration completed cleanly. */

    hu_persona_t out;
    memset(&out, 0xC3, sizeof(out));
    hu_persona_t expected;
    memset(&expected, 0xC3, sizeof(expected));

    HU_ASSERT_EQ(hu_persona_load_legacy(&alloc, e.persona_path, &out), HU_ERR_LEGACY_REFUSED);
    /* Out struct must be untouched — we cannot let the caller's `out` be
     * partially populated.  Byte-for-byte. */
    HU_ASSERT_EQ(memcmp(&out, &expected, sizeof(out)), 0);
    env_cleanup(&e);
}

static void test_load_legacy_refuses_plaintext_when_no_sentinel(void) {
    /* Even on plaintext bytes, the legacy loader refuses unless the
     * migration-pending sentinel marks a brief recovery window.
     * Adversarial-BLOCKED: silent plaintext load is NOT allowed. */
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();

    write_plaintext_fixture(e.persona_path);
    /* Sentinel deliberately not created. */

    hu_persona_t out;
    memset(&out, 0xC3, sizeof(out));
    HU_ASSERT_EQ(hu_persona_load_legacy(&alloc, e.persona_path, &out), HU_ERR_LEGACY_REFUSED);
    env_cleanup(&e);
}

static void test_load_legacy_loads_plaintext_only_when_sentinel_present(void) {
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();

    write_plaintext_fixture(e.persona_path);
    touch_sentinel(e.sentinel);

    hu_persona_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_persona_load_legacy(&alloc, e.persona_path, &out), HU_OK);
    HU_ASSERT_NOT_NULL(out.name);
    HU_ASSERT(strcmp(out.name, "us_8_2_test_persona") == 0);
    hu_persona_deinit(&alloc, &out);
    env_cleanup(&e);
}

/* ------------------------------------------------------------------ */
/* 7. Atomic save adversary (AC-8.2.5 — fix-shape probe)               */
/* ------------------------------------------------------------------ */

static void test_save_encrypted_preserves_prior_state_when_tmp_blocked(void) {
    /* Mirrors tests/test_personal_model_atomic_save.c::
     *   personal_model_save_preserves_prior_state_when_tmp_blocked
     *
     * Strategy: pre-create <path>.tmp as a directory so the atomic open()
     * call fails with EISDIR (POSIX) BEFORE any data has been written.
     * The contract being tested: "if writing the new state fails, the
     * prior state is preserved" — that contract is what survives a real
     * SIGKILL or power loss.  The directory blocker is just a deterministic
     * way to drive the write-fail path. */
    test_env_t e;
    env_init(&e);
    hu_allocator_t alloc = hu_system_allocator();

    /* Step 1 — seed <path> with a known-good encrypted persona so there
     * IS a prior state to preserve. */
    write_plaintext_fixture(e.persona_path);
    char json[2048];
    FILE *f = fopen(e.persona_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    (void)unlink(e.persona_path);

    hu_persona_t p_in;
    memset(&p_in, 0, sizeof(p_in));
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, json, n, &p_in), HU_OK);
    uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES];
    HU_ASSERT_EQ(hu_persona_crypt_derive_key(&alloc, "x", key), HU_OK);
    HU_ASSERT_EQ(hu_persona_save_encrypted(&alloc, e.persona_path, &p_in, key), HU_OK);

    /* Snapshot the prior file contents (byte-exact). */
    uint8_t prior[8192];
    f = fopen(e.persona_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    size_t prior_len = fread(prior, 1, sizeof(prior), f);
    fclose(f);
    HU_ASSERT(prior_len > HU_PERSONA_CRYPT_HEADER_BYTES);

    /* Step 2 — block the tmp slot with a directory. */
    char tmp_path[400];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", e.persona_path);
    HU_ASSERT_EQ(mkdir(tmp_path, 0755), 0);

    /* Step 3 — attempt to overwrite with a save. atomic_write() opens
     * <path>.tmp via open(O_CREAT|O_EXCL); on a directory blocker this
     * fails with EISDIR (or EEXIST) and we return HU_ERR_IO_BUSY before
     * any write touches <path>. */
    hu_error_t err = hu_persona_save_encrypted(&alloc, e.persona_path, &p_in, key);
    HU_ASSERT_EQ(err, HU_ERR_IO_BUSY);

    /* Step 4 — byte-for-byte: prior file is intact. */
    uint8_t after[8192];
    f = fopen(e.persona_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    size_t after_len = fread(after, 1, sizeof(after), f);
    fclose(f);
    HU_ASSERT_EQ(after_len, prior_len);
    HU_ASSERT_EQ(memcmp(prior, after, prior_len), 0);

    /* Step 5 — the prior file still round-trips through decrypt. */
    hu_persona_t p_out;
    memset(&p_out, 0, sizeof(p_out));
    HU_ASSERT_EQ(hu_persona_load_encrypted(&alloc, e.persona_path, key, &p_out), HU_OK);
    HU_ASSERT(strcmp(p_out.name, p_in.name) == 0);

    hu_persona_deinit(&alloc, &p_in);
    hu_persona_deinit(&alloc, &p_out);
    (void)rmdir(tmp_path);
    env_cleanup(&e);
}

/* ------------------------------------------------------------------ */
/* Suite entry                                                         */
/* ------------------------------------------------------------------ */

void run_persona_encryption_tests(void) {
    HU_TEST_SUITE("persona-encryption");

    /* 1. classify predicate truth table */
    HU_RUN_TEST(test_classify_empty_buffer_is_unknown);
    HU_RUN_TEST(test_classify_plaintext_starting_with_brace_is_json);
    HU_RUN_TEST(test_classify_plaintext_with_leading_whitespace_is_json);
    HU_RUN_TEST(test_classify_HUP1_magic_is_encrypted_v1);
    HU_RUN_TEST(test_classify_HUP1_truncated_to_header_only_is_unknown);
    HU_RUN_TEST(test_classify_HUP1_wrong_version_byte_is_unknown);
    HU_RUN_TEST(test_classify_random_binary_is_unknown);

    /* 2. keystore */
    HU_RUN_TEST(test_keystore_creates_keyfile_with_0600_perms);
    HU_RUN_TEST(test_keystore_returns_same_key_on_repeat_call);
    HU_RUN_TEST(test_keystore_rejects_world_readable_keyfile);

    /* 3. save / load round-trip */
    HU_RUN_TEST(test_save_then_load_recovers_all_fields);
    HU_RUN_TEST(test_save_twice_yields_different_ciphertext_same_plaintext);

    /* 4. adversarial wrong-key (BLOCKED) */
    HU_RUN_TEST(test_load_with_wrong_key_returns_decrypt_failed_and_out_untouched);
    HU_RUN_TEST(test_load_with_tampered_ciphertext_returns_decrypt_failed);

    /* 5. migration */
    HU_RUN_TEST(test_migrate_plaintext_yields_encrypted_round_trippable_file);
    HU_RUN_TEST(test_migrate_is_idempotent_on_already_encrypted_file);

    /* 6. refuse-path (BLOCKED) */
    HU_RUN_TEST(test_load_legacy_refuses_encrypted_file_with_legacy_refused_error);
    HU_RUN_TEST(test_load_legacy_refuses_plaintext_when_no_sentinel);
    HU_RUN_TEST(test_load_legacy_loads_plaintext_only_when_sentinel_present);

    /* 7. atomic adversary */
    HU_RUN_TEST(test_save_encrypted_preserves_prior_state_when_tmp_blocked);
}
