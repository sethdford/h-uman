#ifndef HU_CRYPTO_H
#define HU_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

void hu_chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                         const uint8_t *in, uint8_t *out, size_t len);

void hu_chacha20_decrypt(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                         const uint8_t *in, uint8_t *out, size_t len);

void hu_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

void hu_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]);

#endif /* HU_CRYPTO_H */
