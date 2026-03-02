#ifndef SC_CRYPTO_H
#define SC_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

void sc_chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                         uint32_t counter, const uint8_t *in, uint8_t *out,
                         size_t len);

void sc_chacha20_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                         uint32_t counter, const uint8_t *in, uint8_t *out,
                         size_t len);

void sc_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

void sc_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]);

#endif /* SC_CRYPTO_H */
