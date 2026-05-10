/* W15 envelope wrapper — round-trip + backward-compat tests.
 *
 * Exercises hu_encrypted_store_wrap / unwrap / is_encrypted as a
 * standalone unit. No SQLite, no network, no process spawning.
 * The only side effect is a per-pid keystore directory under /tmp,
 * mirrored from tests/test_w15_keystore.c so tombstones and salt
 * files don't bleed across CI runs. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/encrypted_store.h"
#include "human/security/keystore.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

/* Portable byte-buffer search; the system memmem is a GNU extension
 * and isn't always declared in our test compile flags. Used to prove
 * the wrapped envelope doesn't leak the plaintext bytes verbatim. */
static const void *find_bytes(const void *hay, size_t hay_len,
                              const void *needle, size_t needle_len) {
    if (!hay || !needle || needle_len == 0 || hay_len < needle_len)
        return NULL;
    const unsigned char *h = (const unsigned char *)hay;
    const unsigned char *n = (const unsigned char *)needle;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needle_len) == 0)
            return h + i;
    }
    return NULL;
}

/* Pin keystore directory to /tmp/hu-encstore-<pid> so artifacts
 * from prior runs (or other test binaries) cannot influence the
 * passphrase derivation. */
static void set_ks_dir(void) {
    static char dir[128];
    snprintf(dir, sizeof(dir), "/tmp/hu-encstore-%d", (int)getpid());
    setenv("HU_KEYSTORE_DIR", dir, 1);
    (void)mkdir(dir, 0700);
}

static void cleanup_user(const char *user_id) {
    char path[256];
    const char *dir = getenv("HU_KEYSTORE_DIR");
    if (!dir)
        dir = "/tmp";
    snprintf(path, sizeof(path), "%s/%s.tomb", dir, user_id);
    (void)remove(path);
    snprintf(path, sizeof(path), "%s/%s.salt", dir, user_id);
    (void)remove(path);
    snprintf(path, sizeof(path), "%s/%s.kdf", dir, user_id);
    (void)remove(path);
}

static hu_keystore_t *unlock_ks(const char *user_id, const char *pp) {
    set_ks_dir();
    cleanup_user(user_id);
    hu_keystore_t *ks = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), user_id, &ks), HU_OK);
    HU_ASSERT_NOT_NULL(ks);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks, pp, strlen(pp)), HU_OK);
    return ks;
}

/* ── round-trip cases ───────────────────────────────────────────────────── */

static void test_wrap_roundtrip_empty_passes(void) {
    hu_keystore_t *ks = unlock_ks("enc_empty", "passphrase");

    void *ct = NULL;
    size_t ct_len = 0;
    HU_ASSERT_EQ(hu_encrypted_store_wrap(ks, A(), "", 0, &ct, &ct_len), HU_OK);
    HU_ASSERT_NOT_NULL(ct);
    /* Magic (4) + AEAD overhead (>= 12 nonce + 16/32 tag) means the
     * envelope MUST be larger than the magic itself even for an
     * empty plaintext. */
    HU_ASSERT_GT(ct_len, (size_t)HU_ENCRYPTED_STORE_MAGIC_LEN);
    HU_ASSERT(hu_encrypted_store_is_encrypted(ct, ct_len));

    void *pt = NULL;
    size_t pt_len = 12345; /* poisoned sentinel; unwrap must overwrite */
    HU_ASSERT_EQ(hu_encrypted_store_unwrap(ks, ct, ct_len, &pt, &pt_len), HU_OK);
    HU_ASSERT_NOT_NULL(pt);
    HU_ASSERT_EQ(pt_len, (size_t)0);

    A()->free(A()->ctx, pt, pt_len + 1);
    A()->free(A()->ctx, ct, ct_len);
    hu_keystore_close(ks, A());
    cleanup_user("enc_empty");
}

static void test_wrap_roundtrip_short_string(void) {
    hu_keystore_t *ks = unlock_ks("enc_short", "passphrase");

    const char *plaintext = "hello, encrypted memory";
    size_t plain_len = strlen(plaintext);

    void *ct = NULL;
    size_t ct_len = 0;
    HU_ASSERT_EQ(hu_encrypted_store_wrap(ks, A(), plaintext, plain_len, &ct, &ct_len), HU_OK);
    HU_ASSERT_NOT_NULL(ct);
    HU_ASSERT_GT(ct_len, plain_len);

    /* The envelope must NOT contain the plaintext anywhere — even if
     * the AEAD failed silently (it shouldn't, but defense in depth)
     * we'd catch the obvious leak here. */
    HU_ASSERT(find_bytes(ct, ct_len, plaintext, plain_len) == NULL);

    void *pt = NULL;
    size_t pt_len = 0;
    HU_ASSERT_EQ(hu_encrypted_store_unwrap(ks, ct, ct_len, &pt, &pt_len), HU_OK);
    HU_ASSERT_EQ(pt_len, plain_len);
    HU_ASSERT_EQ(memcmp(pt, plaintext, plain_len), 0);

    A()->free(A()->ctx, pt, pt_len + 1);
    A()->free(A()->ctx, ct, ct_len);
    hu_keystore_close(ks, A());
    cleanup_user("enc_short");
}

static void test_wrap_roundtrip_64kb_blob(void) {
    hu_keystore_t *ks = unlock_ks("enc_big", "passphrase");

    /* Fill 64 KiB with a deterministic but non-trivial pattern so a
     * partial truncation in wrap or unwrap fails the memcmp. */
    const size_t blob_len = 64 * 1024;
    unsigned char *blob = (unsigned char *)A()->alloc(A()->ctx, blob_len);
    HU_ASSERT_NOT_NULL(blob);
    for (size_t i = 0; i < blob_len; i++)
        blob[i] = (unsigned char)((i * 31 + 7) & 0xFF);

    void *ct = NULL;
    size_t ct_len = 0;
    HU_ASSERT_EQ(hu_encrypted_store_wrap(ks, A(), blob, blob_len, &ct, &ct_len), HU_OK);
    HU_ASSERT_NOT_NULL(ct);
    HU_ASSERT_GT(ct_len, blob_len);

    void *pt = NULL;
    size_t pt_len = 0;
    HU_ASSERT_EQ(hu_encrypted_store_unwrap(ks, ct, ct_len, &pt, &pt_len), HU_OK);
    HU_ASSERT_EQ(pt_len, blob_len);
    HU_ASSERT_EQ(memcmp(pt, blob, blob_len), 0);

    A()->free(A()->ctx, pt, pt_len + 1);
    A()->free(A()->ctx, ct, ct_len);
    A()->free(A()->ctx, blob, blob_len);
    hu_keystore_close(ks, A());
    cleanup_user("enc_big");
}

/* ── failure / sniff cases ──────────────────────────────────────────────── */

static void test_unwrap_corrupted_ciphertext_fails(void) {
    hu_keystore_t *ks = unlock_ks("enc_corrupt", "passphrase");

    const char *plaintext = "tamper me";
    void *ct = NULL;
    size_t ct_len = 0;
    HU_ASSERT_EQ(hu_encrypted_store_wrap(ks, A(), plaintext, strlen(plaintext), &ct, &ct_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(ct);

    /* Flip a byte deep inside the AEAD payload (skip past magic +
     * nonce so we hit the ciphertext / tag). The keystore must
     * detect the tampering and refuse to decrypt. */
    HU_ASSERT_GT(ct_len, (size_t)(HU_ENCRYPTED_STORE_MAGIC_LEN + 16));
    ((unsigned char *)ct)[ct_len - 4] ^= 0x42;

    void *pt = NULL;
    size_t pt_len = 9999;
    hu_error_t err = hu_encrypted_store_unwrap(ks, ct, ct_len, &pt, &pt_len);
    HU_ASSERT_EQ(err, HU_ERR_CRYPTO_DECRYPT);
    HU_ASSERT_NULL(pt);
    HU_ASSERT_EQ(pt_len, (size_t)0);

    A()->free(A()->ctx, ct, ct_len);
    hu_keystore_close(ks, A());
    cleanup_user("enc_corrupt");
}

/* The whole point of the magic-byte sniff: an upgrade can flip
 * `memory.encrypt_at_rest = true` without orphaning rows that
 * were stored before the keystore existed. unwrap() will refuse
 * a non-magic blob (HU_ERR_INVALID_ARGUMENT) and the SQLite
 * engine falls back to passing the raw bytes through. */
static void test_unwrap_legacy_plaintext_passes_through_via_sniff(void) {
    hu_keystore_t *ks = unlock_ks("enc_legacy", "passphrase");

    const char *legacy = "I was written before encrypt_at_rest existed";
    size_t legacy_len = strlen(legacy);

    HU_ASSERT(!hu_encrypted_store_is_encrypted(legacy, legacy_len));

    /* Calling unwrap on a non-magic blob is a programming error from
     * the wrapper's perspective. The contract is: callers MUST sniff
     * first, then either unwrap or pass through. */
    void *pt = NULL;
    size_t pt_len = 0;
    HU_ASSERT_EQ(hu_encrypted_store_unwrap(ks, legacy, legacy_len, &pt, &pt_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(pt);

    /* Simulate what the SQLite engine actually does: sniff, fail,
     * use the legacy bytes verbatim. */
    const char *bytes_to_use = legacy;
    size_t bytes_len = legacy_len;
    if (hu_encrypted_store_is_encrypted(legacy, legacy_len)) {
        /* unreachable */
        HU_FAIL("legacy plaintext misdetected as encrypted envelope");
    }
    HU_ASSERT_EQ(memcmp(bytes_to_use, legacy, bytes_len), 0);

    hu_keystore_close(ks, A());
    cleanup_user("enc_legacy");
}

static void test_is_encrypted_detects_magic(void) {
    /* A buffer that begins with the literal magic should sniff true
     * regardless of what follows. The full unwrap is what
     * authenticates; the sniff is intentionally cheap. */
    const unsigned char magic_only[] = {'H', 'U', 'E', '1', 0xDE, 0xAD};
    HU_ASSERT(hu_encrypted_store_is_encrypted(magic_only, sizeof(magic_only)));

    /* Exactly the magic length still counts. */
    const unsigned char exact_magic[] = {'H', 'U', 'E', '1'};
    HU_ASSERT(hu_encrypted_store_is_encrypted(exact_magic, sizeof(exact_magic)));
}

static void test_is_encrypted_rejects_non_magic(void) {
    HU_ASSERT(!hu_encrypted_store_is_encrypted(NULL, 0));
    HU_ASSERT(!hu_encrypted_store_is_encrypted("HUE", 3));     /* short */
    HU_ASSERT(!hu_encrypted_store_is_encrypted("HUe1xxx", 7)); /* wrong case */
    HU_ASSERT(!hu_encrypted_store_is_encrypted("hello", 5));
    HU_ASSERT(!hu_encrypted_store_is_encrypted("", 0));

    /* All-zero buffer must NOT match — it would be a poor failure
     * mode if a freshly-zeroed allocation got mis-interpreted as a
     * (truncated) envelope. */
    const unsigned char zeros[16] = {0};
    HU_ASSERT(!hu_encrypted_store_is_encrypted(zeros, sizeof(zeros)));
}

/* ── extra coverage: invalid args + wrong-key separation ────────────────── */

static void test_wrap_rejects_invalid_args(void) {
    hu_keystore_t *ks = unlock_ks("enc_args", "passphrase");
    void *ct = NULL;
    size_t ct_len = 0;

    HU_ASSERT_EQ(hu_encrypted_store_wrap(NULL, A(), "x", 1, &ct, &ct_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_encrypted_store_wrap(ks, NULL, "x", 1, &ct, &ct_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_encrypted_store_wrap(ks, A(), "x", 1, NULL, &ct_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_encrypted_store_wrap(ks, A(), "x", 1, &ct, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_encrypted_store_wrap(ks, A(), NULL, 1, &ct, &ct_len),
                 HU_ERR_INVALID_ARGUMENT);

    hu_keystore_close(ks, A());
    cleanup_user("enc_args");
}

/* Two users with different passphrases produce ciphertexts that
 * each side cannot unwrap. Cross-user replay is not a sniff
 * problem (both blobs have the magic) — it's an AEAD problem
 * surfaced as HU_ERR_CRYPTO_DECRYPT. */
static void test_unwrap_with_other_users_key_fails(void) {
    set_ks_dir();
    cleanup_user("enc_alice");
    cleanup_user("enc_bob");

    hu_keystore_t *alice = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "enc_alice", &alice), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(alice, "alicepass", 9), HU_OK);

    hu_keystore_t *bob = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "enc_bob", &bob), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(bob, "bobpass", 7), HU_OK);

    const char *secret = "alice-only data";
    void *ct = NULL;
    size_t ct_len = 0;
    HU_ASSERT_EQ(hu_encrypted_store_wrap(alice, A(), secret, strlen(secret), &ct, &ct_len),
                 HU_OK);

    /* Bob can sniff — magic still matches — but cannot unwrap. */
    HU_ASSERT(hu_encrypted_store_is_encrypted(ct, ct_len));
    void *pt = NULL;
    size_t pt_len = 0;
    HU_ASSERT_EQ(hu_encrypted_store_unwrap(bob, ct, ct_len, &pt, &pt_len),
                 HU_ERR_CRYPTO_DECRYPT);
    HU_ASSERT_NULL(pt);

    A()->free(A()->ctx, ct, ct_len);
    hu_keystore_close(alice, A());
    hu_keystore_close(bob, A());
    cleanup_user("enc_alice");
    cleanup_user("enc_bob");
}

/* ── suite entry point ──────────────────────────────────────────────────── */

void run_encrypted_store_tests(void) {
    HU_TEST_SUITE("encrypted_store");
    HU_RUN_TEST(test_wrap_roundtrip_empty_passes);
    HU_RUN_TEST(test_wrap_roundtrip_short_string);
    HU_RUN_TEST(test_wrap_roundtrip_64kb_blob);
    HU_RUN_TEST(test_unwrap_corrupted_ciphertext_fails);
    HU_RUN_TEST(test_unwrap_legacy_plaintext_passes_through_via_sniff);
    HU_RUN_TEST(test_is_encrypted_detects_magic);
    HU_RUN_TEST(test_is_encrypted_rejects_non_magic);
    HU_RUN_TEST(test_wrap_rejects_invalid_args);
    HU_RUN_TEST(test_unwrap_with_other_users_key_fails);
}
