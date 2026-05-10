/* W15 — adversarial tests for cryptographic keystore + audit log.
 *
 * Runs against the deterministic placeholder AEAD implementation.
 * No real network, no process spawning, no hardware I/O.
 *
 * All tests set HU_KEYSTORE_DIR to a process-specific temp path so
 * tombstone files don't bleed across test runs. */

#include "human/security/keystore.h"
#include "human/security/audit_log.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HU_ENABLE_SQLITE

/* ── helpers ────────────────────────────────────────────────────────────── */

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

/* Set HU_KEYSTORE_DIR to /tmp/hu-ks-<pid> so tombstones are isolated. */
static void set_ks_dir(void) {
    static char dir[128];
    snprintf(dir, sizeof(dir), "/tmp/hu-ks-%d", (int)getpid());
    setenv("HU_KEYSTORE_DIR", dir, 1);
}

/* Remove a tombstone file for `user_id` (cleanup between tests). */
static void remove_tombstone(const char *user_id) {
    char path[256];
    const char *dir = getenv("HU_KEYSTORE_DIR");
    if (!dir) dir = "/tmp";
    snprintf(path, sizeof(path), "%s/%s.tomb", dir, user_id);
    (void)remove(path);
}

/* Remove a salt file for `user_id` (cleanup between tests). */
static void remove_salt(const char *user_id) {
    char path[256];
    const char *dir = getenv("HU_KEYSTORE_DIR");
    if (!dir) dir = "/tmp";
    snprintf(path, sizeof(path), "%s/%s.salt", dir, user_id);
    (void)remove(path);
}

/* ── keystore tests ─────────────────────────────────────────────────────── */

static void test_w15_keystore_open_and_lock(void) {
    set_ks_dir();
    hu_keystore_t *ks = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_lock_test", &ks), HU_OK);
    HU_ASSERT_NOT_NULL(ks);

    hu_keystore_status_t st;
    HU_ASSERT_EQ(hu_keystore_status(ks, &st), HU_OK);
    HU_ASSERT(!st.master_key_present);
    HU_ASSERT(!st.keychain_available);

    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks, "s3cr3t", 6), HU_OK);

    HU_ASSERT_EQ(hu_keystore_status(ks, &st), HU_OK);
    HU_ASSERT(st.master_key_present);

    hu_keystore_close(ks, A());
}

static void test_w15_passphrase_derivation_deterministic(void) {
    set_ks_dir();
    /* Encrypt under first keystore instance. */
    hu_keystore_t *ks1 = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_det", &ks1), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks1, "passphrase", 10), HU_OK);

    const char *pt = "hello determinism";
    void *ct = NULL;
    size_t ct_len = 0;
    HU_ASSERT_EQ(hu_keystore_encrypt(ks1, "entities", pt, strlen(pt), &ct, &ct_len), HU_OK);
    HU_ASSERT_NOT_NULL(ct);
    HU_ASSERT_GT(ct_len, (size_t)strlen(pt));
    hu_keystore_close(ks1, A());

    /* Re-derive with identical passphrase + user_id → must decrypt successfully. */
    hu_keystore_t *ks2 = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_det", &ks2), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks2, "passphrase", 10), HU_OK);

    void *plaintext = NULL;
    size_t pt_len = 0;
    HU_ASSERT_EQ(hu_keystore_decrypt(ks2, "entities", ct, ct_len, &plaintext, &pt_len), HU_OK);
    HU_ASSERT_NOT_NULL(plaintext);
    HU_ASSERT_EQ(pt_len, strlen(pt));
    HU_ASSERT_EQ(memcmp(plaintext, pt, pt_len), 0);

    A()->free(A()->ctx, plaintext, pt_len + 1);
    A()->free(A()->ctx, ct, ct_len);
    hu_keystore_close(ks2, A());
}

static void test_w15_encrypt_decrypt_round_trip(void) {
    set_ks_dir();
    hu_keystore_t *ks = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_rtrip", &ks), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks, "mypassword", 10), HU_OK);

    const char *original = "The quick brown fox jumps over the lazy dog.";
    size_t orig_len = strlen(original);

    void *ct = NULL;
    size_t ct_len = 0;
    HU_ASSERT_EQ(hu_keystore_encrypt(ks, "relations", original, orig_len, &ct, &ct_len), HU_OK);
    HU_ASSERT_NOT_NULL(ct);
    /* Ciphertext must be longer than plaintext (nonce + hmac overhead). */
    HU_ASSERT_GT(ct_len, orig_len);

    void *recovered = NULL;
    size_t rec_len = 0;
    HU_ASSERT_EQ(hu_keystore_decrypt(ks, "relations", ct, ct_len, &recovered, &rec_len), HU_OK);
    HU_ASSERT_NOT_NULL(recovered);
    HU_ASSERT_EQ(rec_len, orig_len);
    HU_ASSERT_EQ(memcmp(recovered, original, orig_len), 0);

    A()->free(A()->ctx, recovered, rec_len + 1);
    A()->free(A()->ctx, ct, ct_len);
    hu_keystore_close(ks, A());
}

static void test_w15_decrypt_with_wrong_key_fails(void) {
    set_ks_dir();
    /* Encrypt under passphrase A. */
    hu_keystore_t *ks_a = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_wrongkey", &ks_a), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks_a, "correcthorsebattery", 19), HU_OK);

    const char *secret = "top secret memory";
    void *ct = NULL;
    size_t ct_len = 0;
    HU_ASSERT_EQ(hu_keystore_encrypt(ks_a, "entities", secret, strlen(secret), &ct, &ct_len),
                 HU_OK);
    hu_keystore_close(ks_a, A());

    /* Attempt decrypt with a different passphrase → authentication must fail. */
    hu_keystore_t *ks_b = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_wrongkey", &ks_b), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks_b, "wrongpassword", 13), HU_OK);

    void *out_pt = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_keystore_decrypt(ks_b, "entities", ct, ct_len, &out_pt, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_CRYPTO_DECRYPT);
    HU_ASSERT_NULL(out_pt);

    A()->free(A()->ctx, ct, ct_len);
    hu_keystore_close(ks_b, A());
}

static void test_w15_cryptographic_forgetting_unrecoverable(void) {
    set_ks_dir();
    /* Guarantee no stale tombstone from a prior run. */
    remove_tombstone("user_forget");

    /* Encrypt some data. */
    hu_keystore_t *ks = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_forget", &ks), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks, "secretphrase", 12), HU_OK);

    const char *payload = "private memory contents";
    void *ct = NULL;
    size_t ct_len = 0;
    HU_ASSERT_EQ(hu_keystore_encrypt(ks, "blobs", payload, strlen(payload), &ct, &ct_len),
                 HU_OK);
    hu_keystore_close(ks, A());

    /* Destroy the master key — writes tombstone. */
    HU_ASSERT_EQ(hu_keystore_destroy_master_key("user_forget"), HU_OK);

    /* Attempt to re-derive the key with the same passphrase → tombstone blocks it. */
    hu_keystore_t *ks2 = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_forget", &ks2), HU_OK);
    hu_error_t unlock_err = hu_keystore_unlock_with_passphrase(ks2, "secretphrase", 12);
    HU_ASSERT_EQ(unlock_err, HU_ERR_CRYPTO_DECRYPT);

    /* Consequently, decryption must also fail (keystore is still locked). */
    void *out_pt = NULL;
    size_t out_len = 0;
    hu_error_t dec_err = hu_keystore_decrypt(ks2, "blobs", ct, ct_len, &out_pt, &out_len);
    HU_ASSERT_EQ(dec_err, HU_ERR_CRYPTO_DECRYPT);
    HU_ASSERT_NULL(out_pt);

    A()->free(A()->ctx, ct, ct_len);
    hu_keystore_close(ks2, A());

    /* Cleanup tombstone so subsequent runs are clean. */
    remove_tombstone("user_forget");
}

static void test_w15_locked_keystore_encrypt_fails(void) {
    set_ks_dir();
    hu_keystore_t *ks = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_lockfail", &ks), HU_OK);
    /* Do NOT unlock. */

    void *ct = NULL;
    size_t ct_len = 0;
    hu_error_t err = hu_keystore_encrypt(ks, "entities", "data", 4, &ct, &ct_len);
    HU_ASSERT_EQ(err, HU_ERR_CRYPTO_ENCRYPT);
    HU_ASSERT_NULL(ct);

    hu_keystore_close(ks, A());
}

/* W15 production crypto — encrypting the same plaintext twice MUST yield
 * different ciphertexts because the per-call nonce is now cryptographically
 * random. Both ciphertexts must still decrypt to the same plaintext. */
static void test_w15_random_nonce_makes_ciphertext_unique(void) {
    set_ks_dir();
    remove_tombstone("user_nonce");
    remove_salt("user_nonce");

    hu_keystore_t *ks = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_nonce", &ks), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks, "passphrase", 10), HU_OK);

    const char *pt = "same plaintext, different ciphertexts";
    size_t pt_len = strlen(pt);

    void *ct1 = NULL, *ct2 = NULL;
    size_t ct1_len = 0, ct2_len = 0;
    HU_ASSERT_EQ(hu_keystore_encrypt(ks, "entities", pt, pt_len, &ct1, &ct1_len), HU_OK);
    HU_ASSERT_EQ(hu_keystore_encrypt(ks, "entities", pt, pt_len, &ct2, &ct2_len), HU_OK);

    HU_ASSERT_EQ(ct1_len, ct2_len);
    /* The full blob must differ — the nonce alone is enough. The probability
     * of two 12-byte random values colliding in a single test run is
     * vanishingly small (negligible over the lifetime of CI). */
    HU_ASSERT(memcmp(ct1, ct2, ct1_len) != 0);
    /* The first 12 bytes (nonce) MUST also differ. */
    HU_ASSERT(memcmp(ct1, ct2, 12) != 0);

    /* Both must still decrypt to the same plaintext. */
    void *p1 = NULL, *p2 = NULL;
    size_t p1_len = 0, p2_len = 0;
    HU_ASSERT_EQ(hu_keystore_decrypt(ks, "entities", ct1, ct1_len, &p1, &p1_len), HU_OK);
    HU_ASSERT_EQ(hu_keystore_decrypt(ks, "entities", ct2, ct2_len, &p2, &p2_len), HU_OK);
    HU_ASSERT_EQ(p1_len, pt_len);
    HU_ASSERT_EQ(p2_len, pt_len);
    HU_ASSERT_EQ(memcmp(p1, pt, pt_len), 0);
    HU_ASSERT_EQ(memcmp(p2, pt, pt_len), 0);

    A()->free(A()->ctx, p1, p1_len + 1);
    A()->free(A()->ctx, p2, p2_len + 1);
    A()->free(A()->ctx, ct1, ct1_len);
    A()->free(A()->ctx, ct2, ct2_len);
    hu_keystore_close(ks, A());
    remove_salt("user_nonce");
}

/* W15 production crypto — different users with the same passphrase MUST
 * derive different master keys (per-user salt defeats rainbow tables). */
static void test_w15_per_user_salt_separates_keys(void) {
    set_ks_dir();
    remove_tombstone("user_alpha");
    remove_tombstone("user_beta");
    remove_salt("user_alpha");
    remove_salt("user_beta");

    /* Alpha encrypts under shared passphrase. */
    hu_keystore_t *ks_a = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_alpha", &ks_a), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks_a, "shared-pp", 9), HU_OK);
    const char *pt = "alpha-only secret";
    void *ct = NULL;
    size_t ct_len = 0;
    HU_ASSERT_EQ(hu_keystore_encrypt(ks_a, "entities", pt, strlen(pt), &ct, &ct_len), HU_OK);
    hu_keystore_close(ks_a, A());

    /* Beta unlocks the SAME passphrase. The salt is per-user so the
     * master key MUST differ; the alpha-encrypted blob MUST NOT decrypt. */
    hu_keystore_t *ks_b = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_beta", &ks_b), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks_b, "shared-pp", 9), HU_OK);

    void *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_keystore_decrypt(ks_b, "entities", ct, ct_len, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_CRYPTO_DECRYPT);
    HU_ASSERT_NULL(out);

    A()->free(A()->ctx, ct, ct_len);
    hu_keystore_close(ks_b, A());
    remove_salt("user_alpha");
    remove_salt("user_beta");
}

/* W15 production crypto — destroying the master key removes the salt;
 * even after manually deleting the tombstone, the same passphrase
 * produces a fresh, different master key (ciphertext is unrecoverable). */
static void test_w15_destruction_removes_salt_makes_recovery_impossible(void) {
    set_ks_dir();
    remove_tombstone("user_destruct");
    remove_salt("user_destruct");

    /* Encrypt under v1 of the master key. */
    hu_keystore_t *ks = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_destruct", &ks), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks, "irrecoverable", 13), HU_OK);
    const char *pt = "data tied to v1 salt";
    void *ct = NULL;
    size_t ct_len = 0;
    HU_ASSERT_EQ(hu_keystore_encrypt(ks, "entities", pt, strlen(pt), &ct, &ct_len), HU_OK);
    hu_keystore_close(ks, A());

    /* Destroy: writes tombstone AND removes the salt. */
    HU_ASSERT_EQ(hu_keystore_destroy_master_key("user_destruct"), HU_OK);

    /* Adversary deletes the tombstone but cannot recover the salt. */
    remove_tombstone("user_destruct");

    /* Re-unlock with the same passphrase succeeds (no tombstone), but a
     * fresh salt is generated → master key v2 ≠ v1 → ct does not decrypt. */
    hu_keystore_t *ks2 = NULL;
    HU_ASSERT_EQ(hu_keystore_open(A(), "user_destruct", &ks2), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks2, "irrecoverable", 13), HU_OK);

    void *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_keystore_decrypt(ks2, "entities", ct, ct_len, &out, &out_len),
                 HU_ERR_CRYPTO_DECRYPT);
    HU_ASSERT_NULL(out);

    A()->free(A()->ctx, ct, ct_len);
    hu_keystore_close(ks2, A());
    remove_salt("user_destruct");
}

static void test_w15_invalid_args_rejected(void) {
    set_ks_dir();
    hu_keystore_t *ks = NULL;

    HU_ASSERT_EQ(hu_keystore_open(NULL, "u", &ks), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_keystore_open(A(), NULL, &ks), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_keystore_open(A(), "", &ks), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_keystore_open(A(), "u", NULL), HU_ERR_INVALID_ARGUMENT);

    HU_ASSERT_EQ(hu_keystore_open(A(), "u_inv", &ks), HU_OK);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(NULL, "pp", 2), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks, NULL, 0), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_keystore_unlock_with_passphrase(ks, "pp", 0), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_keystore_destroy_master_key(NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_keystore_destroy_master_key(""), HU_ERR_INVALID_ARGUMENT);

    hu_keystore_close(ks, A());
}

/* ── audit log tests ────────────────────────────────────────────────────── */

static void test_w15_audit_log_round_trip(void) {
    hu_audit_log_t *log = NULL;
    HU_ASSERT_EQ(hu_audit_log_open(A(), NULL, "user_audit1", &log), HU_OK);
    HU_ASSERT_NOT_NULL(log);

    /* Append three events with explicit timestamps so ordering is stable. */
    hu_audit_event_t ev1 = {
        .operation   = HU_AUDIT_OP_WRITE,
        .kind        = HU_MEM_ENTITY,
        .target_id   = 42,
        .actor       = "agent",
        .occurred_at = 1000,
        .summary     = "upsert entity",
        .contact_id  = "user_audit1",
    };
    hu_audit_event_t ev2 = {
        .operation   = HU_AUDIT_OP_READ,
        .kind        = HU_MEM_RELATION,
        .target_id   = 7,
        .actor       = "user",
        .occurred_at = 2000,
        .summary     = "recall relation",
        .contact_id  = "user_audit1",
    };
    hu_audit_event_t ev3 = {
        .operation   = HU_AUDIT_OP_ERASE,
        .kind        = HU_MEM_ENTITY,
        .target_id   = 42,
        .actor       = "scheduler",
        .occurred_at = 3000,
        .summary     = NULL,
        .contact_id  = "user_audit1",
    };
    HU_ASSERT_EQ(hu_audit_log_append(log, &ev1), HU_OK);
    HU_ASSERT_EQ(hu_audit_log_append(log, &ev2), HU_OK);
    HU_ASSERT_EQ(hu_audit_log_append(log, &ev3), HU_OK);

    /* Query with no filters — should return all three in insertion order. */
    hu_audit_query_t q;
    memset(&q, 0, sizeof(q));
    hu_audit_event_t *results = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_audit_log_query(log, &q, A(), &results, &count), HU_OK);
    HU_ASSERT_EQ(count, (size_t)3);
    HU_ASSERT_NOT_NULL(results);

    HU_ASSERT_EQ(results[0].operation, HU_AUDIT_OP_WRITE);
    HU_ASSERT_EQ(results[0].kind,      HU_MEM_ENTITY);
    HU_ASSERT_EQ(results[0].target_id, (int64_t)42);
    HU_ASSERT_NOT_NULL(results[0].actor);
    HU_ASSERT_EQ(strcmp(results[0].actor, "agent"), 0);
    HU_ASSERT_EQ(results[0].occurred_at, (int64_t)1000);

    HU_ASSERT_EQ(results[1].operation, HU_AUDIT_OP_READ);
    HU_ASSERT_EQ(results[1].kind,      HU_MEM_RELATION);

    HU_ASSERT_EQ(results[2].operation, HU_AUDIT_OP_ERASE);
    HU_ASSERT_NULL(results[2].summary);

    hu_audit_events_free(A(), results, count);
    hu_audit_log_close(log, A());
}

static void test_w15_audit_log_filter_by_actor(void) {
    hu_audit_log_t *log = NULL;
    HU_ASSERT_EQ(hu_audit_log_open(A(), NULL, "user_filter", &log), HU_OK);

    hu_audit_event_t ev_agent = {
        .operation   = HU_AUDIT_OP_WRITE,
        .kind        = HU_MEM_ENTITY,
        .actor       = "agent",
        .occurred_at = 100,
        .contact_id  = "user_filter",
    };
    hu_audit_event_t ev_user = {
        .operation   = HU_AUDIT_OP_READ,
        .kind        = HU_MEM_RELATION,
        .actor       = "user",
        .occurred_at = 200,
        .contact_id  = "user_filter",
    };
    hu_audit_event_t ev_agent2 = {
        .operation   = HU_AUDIT_OP_ERASE,
        .kind        = HU_MEM_ENTITY,
        .actor       = "agent",
        .occurred_at = 300,
        .contact_id  = "user_filter",
    };
    HU_ASSERT_EQ(hu_audit_log_append(log, &ev_agent),  HU_OK);
    HU_ASSERT_EQ(hu_audit_log_append(log, &ev_user),   HU_OK);
    HU_ASSERT_EQ(hu_audit_log_append(log, &ev_agent2), HU_OK);

    /* Filter: only "agent" events. */
    hu_audit_query_t q;
    memset(&q, 0, sizeof(q));
    q.actor = "agent";

    hu_audit_event_t *results = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_audit_log_query(log, &q, A(), &results, &count), HU_OK);
    HU_ASSERT_EQ(count, (size_t)2);
    HU_ASSERT_NOT_NULL(results);
    HU_ASSERT_EQ(strcmp(results[0].actor, "agent"), 0);
    HU_ASSERT_EQ(results[0].operation, HU_AUDIT_OP_WRITE);
    HU_ASSERT_EQ(strcmp(results[1].actor, "agent"), 0);
    HU_ASSERT_EQ(results[1].operation, HU_AUDIT_OP_ERASE);

    hu_audit_events_free(A(), results, count);

    /* Filter: only "user" events — expect exactly 1. */
    memset(&q, 0, sizeof(q));
    q.actor = "user";
    HU_ASSERT_EQ(hu_audit_log_query(log, &q, A(), &results, &count), HU_OK);
    HU_ASSERT_EQ(count, (size_t)1);
    HU_ASSERT_EQ(strcmp(results[0].actor, "user"), 0);
    HU_ASSERT_EQ(results[0].operation, HU_AUDIT_OP_READ);

    hu_audit_events_free(A(), results, count);
    hu_audit_log_close(log, A());
}

static void test_w15_audit_log_empty_query_returns_zero(void) {
    hu_audit_log_t *log = NULL;
    HU_ASSERT_EQ(hu_audit_log_open(A(), NULL, "user_empty", &log), HU_OK);

    hu_audit_query_t q;
    memset(&q, 0, sizeof(q));
    hu_audit_event_t *results = NULL;
    size_t count = 99;
    HU_ASSERT_EQ(hu_audit_log_query(log, &q, A(), &results, &count), HU_OK);
    HU_ASSERT_EQ(count, (size_t)0);
    /* results may be a zero-length allocation; free it properly. */
    hu_audit_events_free(A(), results, count);

    hu_audit_log_close(log, A());
}

static void test_w15_audit_log_invalid_args_rejected(void) {
    HU_ASSERT_EQ(hu_audit_log_open(NULL, NULL, "u", NULL), HU_ERR_INVALID_ARGUMENT);

    hu_audit_log_t *log = NULL;
    HU_ASSERT_EQ(hu_audit_log_open(A(), NULL, "u_inv2", &log), HU_OK);

    HU_ASSERT_EQ(hu_audit_log_append(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_audit_log_append(log,  NULL), HU_ERR_INVALID_ARGUMENT);

    hu_audit_event_t *r = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_audit_log_query(NULL, NULL, A(), &r, &n), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_audit_log_query(log,  NULL, A(), NULL, &n), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_audit_log_query(log,  NULL, A(), &r, NULL), HU_ERR_INVALID_ARGUMENT);

    hu_audit_log_close(log, A());
}

#endif /* HU_ENABLE_SQLITE */

/* ── test suite entry point ─────────────────────────────────────────────── */

void run_w15_keystore_tests(void) {
    HU_TEST_SUITE("W15 cryptographic keystore + audit log");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w15_keystore_open_and_lock);
    HU_RUN_TEST(test_w15_passphrase_derivation_deterministic);
    HU_RUN_TEST(test_w15_encrypt_decrypt_round_trip);
    HU_RUN_TEST(test_w15_decrypt_with_wrong_key_fails);
    HU_RUN_TEST(test_w15_cryptographic_forgetting_unrecoverable);
    HU_RUN_TEST(test_w15_locked_keystore_encrypt_fails);
    HU_RUN_TEST(test_w15_random_nonce_makes_ciphertext_unique);
    HU_RUN_TEST(test_w15_per_user_salt_separates_keys);
    HU_RUN_TEST(test_w15_destruction_removes_salt_makes_recovery_impossible);
    HU_RUN_TEST(test_w15_invalid_args_rejected);
    HU_RUN_TEST(test_w15_audit_log_round_trip);
    HU_RUN_TEST(test_w15_audit_log_filter_by_actor);
    HU_RUN_TEST(test_w15_audit_log_empty_query_returns_zero);
    HU_RUN_TEST(test_w15_audit_log_invalid_args_rejected);
#endif
}
