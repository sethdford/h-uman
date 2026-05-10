/*
 * Encrypted memory envelope — implementation.
 *
 * See include/human/memory/encrypted_store.h for the contract and
 * envelope layout. This file is intentionally small: the heavy
 * cryptography lives in `hu_keystore_t`. Our job is just to prepend
 * a 4-byte magic so a reader can cheaply tell wrapped rows from
 * legacy plaintext rows on the way back out of storage.
 *
 * Threat note: we deliberately do NOT log plaintext, ciphertext,
 * keystore handles, or any envelope metadata. Even error paths use
 * coarse error codes so a failure mode doesn't accidentally become
 * an oracle for an attacker correlating timing/error patterns.
 */

#include "human/memory/encrypted_store.h"

#include <stdint.h>
#include <string.h>

/* Single fixed namespace passed to the keystore so any blob written
 * by wrap() can be unwrapped by unwrap() given the same master key.
 * Per-table keying is intentionally NOT done here — it would tie
 * envelopes to their originating table and break legitimate
 * cross-engine reads (e.g. a tool peeking at a cached row). The
 * keystore still derives a unique data key for this namespace via
 * HMAC(master_key, "memory.row"). */
static const char ENCRYPTED_STORE_NAMESPACE[] = "memory.row";

bool hu_encrypted_store_is_encrypted(const void *blob, size_t blob_len) {
    if (!blob || blob_len < HU_ENCRYPTED_STORE_MAGIC_LEN)
        return false;
    return memcmp(blob, HU_ENCRYPTED_STORE_MAGIC,
                  HU_ENCRYPTED_STORE_MAGIC_LEN) == 0;
}

hu_error_t hu_encrypted_store_wrap(hu_keystore_t *ks, hu_allocator_t *alloc,
                                   const void *plaintext, size_t pt_len,
                                   void **ct_out, size_t *ct_len_out) {
    if (!ks || !alloc || !ct_out || !ct_len_out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!plaintext && pt_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    *ct_out = NULL;
    *ct_len_out = 0;

    void *ks_ct = NULL;
    size_t ks_ct_len = 0;
    hu_error_t err = hu_keystore_encrypt(ks, ENCRYPTED_STORE_NAMESPACE,
                                         plaintext, pt_len, &ks_ct, &ks_ct_len);
    if (err != HU_OK)
        return err;
    if (!ks_ct || ks_ct_len == 0) {
        /* Defensive: keystore should never report HU_OK with no
         * ciphertext. If it ever did, free anything it might have
         * leaked and surface a clean error rather than emitting an
         * envelope with a zero-length payload. */
        if (ks_ct)
            alloc->free(alloc->ctx, ks_ct, ks_ct_len);
        return HU_ERR_CRYPTO_ENCRYPT;
    }

    size_t total = (size_t)HU_ENCRYPTED_STORE_MAGIC_LEN + ks_ct_len;
    uint8_t *buf = (uint8_t *)alloc->alloc(alloc->ctx, total);
    if (!buf) {
        alloc->free(alloc->ctx, ks_ct, ks_ct_len);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(buf, HU_ENCRYPTED_STORE_MAGIC, HU_ENCRYPTED_STORE_MAGIC_LEN);
    memcpy(buf + HU_ENCRYPTED_STORE_MAGIC_LEN, ks_ct, ks_ct_len);
    alloc->free(alloc->ctx, ks_ct, ks_ct_len);

    *ct_out = buf;
    *ct_len_out = total;
    return HU_OK;
}

hu_error_t hu_encrypted_store_unwrap(hu_keystore_t *ks,
                                     const void *ciphertext, size_t ct_len,
                                     void **pt_out, size_t *pt_len_out) {
    if (!ks || !ciphertext || !pt_out || !pt_len_out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!hu_encrypted_store_is_encrypted(ciphertext, ct_len))
        return HU_ERR_INVALID_ARGUMENT;

    *pt_out = NULL;
    *pt_len_out = 0;

    const uint8_t *payload =
        (const uint8_t *)ciphertext + HU_ENCRYPTED_STORE_MAGIC_LEN;
    size_t payload_len = ct_len - HU_ENCRYPTED_STORE_MAGIC_LEN;

    return hu_keystore_decrypt(ks, ENCRYPTED_STORE_NAMESPACE, payload,
                               payload_len, pt_out, pt_len_out);
}
