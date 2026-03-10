#include "human/core/error.h"
#include "human/crypto.h"
#include "human/security.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HU_ENABLE_FIPS_CRYPTO
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#define HU_KEY_LEN 32

static void hu_secure_zero(void *p, size_t n) {
#if defined(__STDC_LIB_EXT1__)
    memset_s(p, n, 0, n);
#elif defined(__GNUC__) || defined(__clang__)
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");
#else
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--)
        *vp++ = 0;
#endif
}
#define HU_NONCE_LEN       12
#define HU_HMAC_LEN        32
#define HU_ENC2_PREFIX     "enc2:"
#define HU_ENC2_PREFIX_LEN 5

struct hu_secret_store {
    char key_path[1024];
    size_t key_path_len;
    bool enabled;
};

#ifndef HU_ENABLE_FIPS_CRYPTO
static int dev_urandom_bytes(uint8_t *buf, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return (n == len) ? 0 : -1;
}
#endif

#ifdef HU_ENABLE_FIPS_CRYPTO
#define HU_AES_GCM_NONCE_LEN 12
#define HU_AES_GCM_TAG_LEN   16
#define HU_AES_GCM_KEY_LEN   32

static int secure_random_bytes(uint8_t *buf, size_t len) {
    return (RAND_bytes(buf, (int)len) == 1) ? 0 : -1;
}

static hu_error_t aes_gcm_encrypt(const unsigned char *key, const unsigned char *plaintext,
                                  size_t pt_len, unsigned char *out, size_t *out_len) {
    unsigned char nonce[HU_AES_GCM_NONCE_LEN];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1)
        return HU_ERR_CRYPTO_ENCRYPT;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;

    hu_error_t err = HU_OK;
    int len = 0;
    int ct_len = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        err = HU_ERR_CRYPTO_ENCRYPT;
        goto cleanup;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, HU_AES_GCM_NONCE_LEN, NULL) != 1) {
        err = HU_ERR_CRYPTO_ENCRYPT;
        goto cleanup;
    }
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) {
        err = HU_ERR_CRYPTO_ENCRYPT;
        goto cleanup;
    }
    if (EVP_EncryptUpdate(ctx, out + HU_AES_GCM_NONCE_LEN, &len, plaintext, (int)pt_len) != 1) {
        err = HU_ERR_CRYPTO_ENCRYPT;
        goto cleanup;
    }
    ct_len = len;
    if (EVP_EncryptFinal_ex(ctx, out + HU_AES_GCM_NONCE_LEN + ct_len, &len) != 1) {
        err = HU_ERR_CRYPTO_ENCRYPT;
        goto cleanup;
    }
    ct_len += len;

    memcpy(out, nonce, HU_AES_GCM_NONCE_LEN);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, HU_AES_GCM_TAG_LEN,
                            out + HU_AES_GCM_NONCE_LEN + ct_len) != 1) {
        err = HU_ERR_CRYPTO_ENCRYPT;
        goto cleanup;
    }

    *out_len = HU_AES_GCM_NONCE_LEN + (size_t)ct_len + HU_AES_GCM_TAG_LEN;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return err;
}

static hu_error_t aes_gcm_decrypt(const unsigned char *key, const unsigned char *in, size_t in_len,
                                  unsigned char *plaintext, size_t *pt_len) {
    if (in_len < HU_AES_GCM_NONCE_LEN + HU_AES_GCM_TAG_LEN)
        return HU_ERR_INVALID_ARGUMENT;

    const unsigned char *nonce = in;
    size_t ct_len = in_len - HU_AES_GCM_NONCE_LEN - HU_AES_GCM_TAG_LEN;
    const unsigned char *ciphertext = in + HU_AES_GCM_NONCE_LEN;
    const unsigned char *tag = in + HU_AES_GCM_NONCE_LEN + ct_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;

    hu_error_t err = HU_OK;
    int len = 0;
    int out_len = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        err = HU_ERR_CRYPTO_DECRYPT;
        goto cleanup;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, HU_AES_GCM_NONCE_LEN, NULL) != 1) {
        err = HU_ERR_CRYPTO_DECRYPT;
        goto cleanup;
    }
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) {
        err = HU_ERR_CRYPTO_DECRYPT;
        goto cleanup;
    }
    if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, (int)ct_len) != 1) {
        err = HU_ERR_CRYPTO_DECRYPT;
        goto cleanup;
    }
    out_len = len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, HU_AES_GCM_TAG_LEN, (void *)tag) != 1) {
        err = HU_ERR_CRYPTO_DECRYPT;
        goto cleanup;
    }
    if (EVP_DecryptFinal_ex(ctx, plaintext + out_len, &len) != 1) {
        err = HU_ERR_CRYPTO_DECRYPT;
        goto cleanup;
    }
    out_len += len;

    *pt_len = (size_t)out_len;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return err;
}
#else
static int secure_random_bytes(uint8_t *buf, size_t len) {
    return dev_urandom_bytes(buf, len);
}
#endif /* HU_ENABLE_FIPS_CRYPTO */

void hu_hex_encode(const uint8_t *data, size_t len, char *out_hex) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out_hex[i * 2] = hex[data[i] >> 4];
        out_hex[i * 2 + 1] = hex[data[i] & 0x0f];
    }
    out_hex[len * 2] = '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

hu_error_t hu_hex_decode(const char *hex, size_t hex_len, uint8_t *out_data, size_t out_cap,
                         size_t *out_len) {
    if (hex_len & 1)
        return HU_ERR_PARSE;
    size_t byte_len = hex_len / 2;
    if (out_cap < byte_len)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < byte_len; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return HU_ERR_PARSE;
        out_data[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = byte_len;
    return HU_OK;
}

bool hu_secret_store_is_encrypted(const char *value) {
    return value && strlen(value) >= HU_ENC2_PREFIX_LEN &&
           memcmp(value, HU_ENC2_PREFIX, HU_ENC2_PREFIX_LEN) == 0;
}

static hu_error_t load_or_create_key(hu_secret_store_t *store, uint8_t key[HU_KEY_LEN]) {
    const char *path = store->key_path;

    FILE *f = fopen(path, "rb");
    if (f) {
        char hex_buf[80];
        size_t n = fread(hex_buf, 1, sizeof(hex_buf) - 1, f);
        fclose(f);
        hex_buf[n] = '\0';
        /* trim */
        while (n > 0 && (hex_buf[n - 1] == ' ' || hex_buf[n - 1] == '\t' ||
                         hex_buf[n - 1] == '\n' || hex_buf[n - 1] == '\r'))
            n--;
        hex_buf[n] = '\0';
        size_t dlen;
        hu_error_t err = hu_hex_decode(hex_buf, strlen(hex_buf), key, HU_KEY_LEN, &dlen);
        if (err != HU_OK || dlen != HU_KEY_LEN) {
            hu_secure_zero(key, HU_KEY_LEN);
            return HU_ERR_CRYPTO_DECRYPT;
        }
        return HU_OK;
    }

    /* Create new key */
    if (secure_random_bytes(key, HU_KEY_LEN) != 0)
        return HU_ERR_CRYPTO_ENCRYPT;

    /* Ensure parent dir exists */
    char *slash = strrchr(store->key_path, '/');
    if (slash && slash > store->key_path) {
        char dir[1024];
        size_t dlen = (size_t)(slash - store->key_path);
        if (dlen < sizeof(dir)) {
            memcpy(dir, store->key_path, dlen);
            dir[dlen] = '\0';
            (void)mkdir(dir, 0755);
        }
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        hu_secure_zero(key, HU_KEY_LEN);
        return HU_ERR_IO;
    }
    f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        hu_secure_zero(key, HU_KEY_LEN);
        return HU_ERR_IO;
    }
    char hex_out[HU_KEY_LEN * 2 + 1];
    hu_hex_encode(key, HU_KEY_LEN, hex_out);
    fwrite(hex_out, 1, HU_KEY_LEN * 2, f);
    fclose(f);
    /* Key left for caller to use; caller must hu_secure_zero when done */
    return HU_OK;
}

hu_secret_store_t *hu_secret_store_create(hu_allocator_t *alloc, const char *config_dir,
                                          bool enabled) {
    if (!alloc || !config_dir)
        return NULL;

    hu_secret_store_t *store =
        (hu_secret_store_t *)alloc->alloc(alloc->ctx, sizeof(hu_secret_store_t));
    if (!store)
        return NULL;
    memset(store, 0, sizeof(*store));
    store->enabled = enabled;

    int n = snprintf(store->key_path, sizeof(store->key_path), "%s/.secret_key", config_dir);
    if (n <= 0 || (size_t)n >= sizeof(store->key_path)) {
        store->key_path[0] = '\0';
    }
    store->key_path_len = strlen(store->key_path);

    return store;
}

void hu_secret_store_destroy(hu_secret_store_t *store, hu_allocator_t *alloc) {
    if (!store || !alloc)
        return;
    alloc->free(alloc->ctx, store, sizeof(hu_secret_store_t));
}

hu_error_t hu_secret_store_encrypt(hu_secret_store_t *store, hu_allocator_t *alloc,
                                   const char *plaintext, char **out_ciphertext_hex) {
    if (!store || !alloc || !out_ciphertext_hex)
        return HU_ERR_INVALID_ARGUMENT;
    *out_ciphertext_hex = NULL;

    if (!store->enabled || !plaintext || !plaintext[0]) {
        size_t len = plaintext ? strlen(plaintext) : 0;
        char *dup = (char *)alloc->alloc(alloc->ctx, len + 1);
        if (!dup)
            return HU_ERR_OUT_OF_MEMORY;
        if (plaintext)
            memcpy(dup, plaintext, len + 1);
        else
            dup[0] = '\0';
        *out_ciphertext_hex = dup;
        return HU_OK;
    }

    uint8_t key[HU_KEY_LEN];
    hu_error_t err = load_or_create_key(store, key);
    if (err != HU_OK)
        return err;

    size_t plen = strlen(plaintext);

#ifndef HU_ENABLE_FIPS_CRYPTO
    uint8_t nonce[HU_NONCE_LEN];
    if (secure_random_bytes(nonce, HU_NONCE_LEN) != 0) {
        hu_secure_zero(key, HU_KEY_LEN);
        return HU_ERR_CRYPTO_ENCRYPT;
    }

    uint8_t *ciphertext = (uint8_t *)alloc->alloc(alloc->ctx, plen + HU_HMAC_LEN);
    if (!ciphertext)
        return HU_ERR_OUT_OF_MEMORY;

    hu_chacha20_encrypt(key, nonce, 1, (const uint8_t *)plaintext, ciphertext, plen);

    uint8_t hmac[32];
    hu_hmac_sha256(key, HU_KEY_LEN, ciphertext, plen, hmac);

    size_t blob_len = HU_NONCE_LEN + plen + HU_HMAC_LEN;
    uint8_t *blob = (uint8_t *)alloc->alloc(alloc->ctx, blob_len);
    if (!blob) {
        alloc->free(alloc->ctx, ciphertext, plen + HU_HMAC_LEN);
        hu_secure_zero(key, HU_KEY_LEN);
        hu_secure_zero(nonce, HU_NONCE_LEN);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(blob, nonce, HU_NONCE_LEN);
    memcpy(blob + HU_NONCE_LEN, ciphertext, plen);
    memcpy(blob + HU_NONCE_LEN + plen, hmac, HU_HMAC_LEN);
    alloc->free(alloc->ctx, ciphertext, plen + HU_HMAC_LEN);

    size_t hex_len = blob_len * 2 + HU_ENC2_PREFIX_LEN + 1;
    char *hex_out = (char *)alloc->alloc(alloc->ctx, hex_len);
    if (!hex_out) {
        alloc->free(alloc->ctx, blob, blob_len);
        hu_secure_zero(key, HU_KEY_LEN);
        hu_secure_zero(nonce, HU_NONCE_LEN);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(hex_out, HU_ENC2_PREFIX, HU_ENC2_PREFIX_LEN);
    hu_hex_encode(blob, blob_len, hex_out + HU_ENC2_PREFIX_LEN);
    alloc->free(alloc->ctx, blob, blob_len);

    hu_secure_zero(key, HU_KEY_LEN);
    hu_secure_zero(nonce, HU_NONCE_LEN);
#else
    size_t blob_len = HU_AES_GCM_NONCE_LEN + plen + HU_AES_GCM_TAG_LEN;
    uint8_t *blob = (uint8_t *)alloc->alloc(alloc->ctx, blob_len);
    if (!blob) {
        hu_secure_zero(key, HU_KEY_LEN);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t out_len = blob_len;
    err = aes_gcm_encrypt(key, (const unsigned char *)plaintext, plen, blob, &out_len);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, blob, blob_len);
        hu_secure_zero(key, HU_KEY_LEN);
        return err;
    }
    blob_len = out_len;

    size_t hex_len = blob_len * 2 + HU_ENC2_PREFIX_LEN + 1;
    char *hex_out = (char *)alloc->alloc(alloc->ctx, hex_len);
    if (!hex_out) {
        alloc->free(alloc->ctx, blob, blob_len);
        hu_secure_zero(key, HU_KEY_LEN);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(hex_out, HU_ENC2_PREFIX, HU_ENC2_PREFIX_LEN);
    hu_hex_encode(blob, blob_len, hex_out + HU_ENC2_PREFIX_LEN);
    alloc->free(alloc->ctx, blob, blob_len);

    hu_secure_zero(key, HU_KEY_LEN);
#endif /* HU_ENABLE_FIPS_CRYPTO */

    *out_ciphertext_hex = hex_out;
    return HU_OK;
}

hu_error_t hu_secret_store_decrypt(hu_secret_store_t *store, hu_allocator_t *alloc,
                                   const char *value, char **out_plaintext) {
    if (!store || !alloc || !out_plaintext)
        return HU_ERR_INVALID_ARGUMENT;
    *out_plaintext = NULL;

    if (!value)
        return HU_ERR_INVALID_ARGUMENT;

    if (!hu_secret_store_is_encrypted(value)) {
        char *dup = (char *)alloc->alloc(alloc->ctx, strlen(value) + 1);
        if (!dup)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(dup, value, strlen(value) + 1);
        *out_plaintext = dup;
        return HU_OK;
    }

    const char *hex_str = value + HU_ENC2_PREFIX_LEN;
    size_t hex_len = strlen(hex_str);
    if (hex_len & 1)
        return HU_ERR_CRYPTO_DECRYPT;

    size_t blob_len = hex_len / 2;
    if (blob_len <= HU_NONCE_LEN)
        return HU_ERR_CRYPTO_DECRYPT;

    uint8_t blob[8192];
    if (blob_len > sizeof(blob))
        return HU_ERR_CRYPTO_DECRYPT;

    size_t decoded;
    hu_error_t err = hu_hex_decode(hex_str, hex_len, blob, sizeof(blob), &decoded);
    if (err != HU_OK || decoded != blob_len)
        return HU_ERR_CRYPTO_DECRYPT;

    uint8_t key[HU_KEY_LEN];
    err = load_or_create_key(store, key);
    if (err != HU_OK)
        return err;

#ifndef HU_ENABLE_FIPS_CRYPTO
    size_t ct_len = blob_len - HU_NONCE_LEN - HU_HMAC_LEN;
    uint8_t computed_hmac[32];
    hu_hmac_sha256(key, HU_KEY_LEN, blob + HU_NONCE_LEN, ct_len, computed_hmac);

    /* Constant-time compare */
    unsigned char diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= computed_hmac[i] ^ blob[HU_NONCE_LEN + ct_len + i];
    if (diff != 0) {
        hu_secure_zero(key, HU_KEY_LEN);
        return HU_ERR_CRYPTO_DECRYPT;
    }

    uint8_t plain_buf[8192];
    if (ct_len > sizeof(plain_buf))
        return HU_ERR_CRYPTO_DECRYPT;

    hu_chacha20_decrypt(key, blob, 1, blob + HU_NONCE_LEN, plain_buf, ct_len);

    char *plain = (char *)alloc->alloc(alloc->ctx, ct_len + 1);
    if (!plain) {
        hu_secure_zero(key, HU_KEY_LEN);
        hu_secure_zero(plain_buf, ct_len);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(plain, plain_buf, ct_len);
    plain[ct_len] = '\0';
    hu_secure_zero(key, HU_KEY_LEN);
    hu_secure_zero(plain_buf, ct_len);
#else
    if (blob_len < HU_AES_GCM_NONCE_LEN + HU_AES_GCM_TAG_LEN) {
        hu_secure_zero(key, HU_KEY_LEN);
        return HU_ERR_CRYPTO_DECRYPT;
    }
    size_t pt_len = 0;
    uint8_t plain_buf[8192];
    if (blob_len - HU_AES_GCM_NONCE_LEN - HU_AES_GCM_TAG_LEN > sizeof(plain_buf)) {
        hu_secure_zero(key, HU_KEY_LEN);
        return HU_ERR_CRYPTO_DECRYPT;
    }
    err = aes_gcm_decrypt(key, blob, blob_len, plain_buf, &pt_len);
    if (err != HU_OK) {
        hu_secure_zero(key, HU_KEY_LEN);
        return err;
    }
    char *plain = (char *)alloc->alloc(alloc->ctx, pt_len + 1);
    if (!plain) {
        hu_secure_zero(key, HU_KEY_LEN);
        hu_secure_zero(plain_buf, pt_len);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(plain, plain_buf, pt_len);
    plain[pt_len] = '\0';
    hu_secure_zero(key, HU_KEY_LEN);
    hu_secure_zero(plain_buf, pt_len);
#endif /* HU_ENABLE_FIPS_CRYPTO */

    *out_plaintext = plain;
    return HU_OK;
}
