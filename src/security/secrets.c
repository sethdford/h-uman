#include "seaclaw/security.h"
#include <stdint.h>
#include "seaclaw/core/error.h"
#include "crypto.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define SC_KEY_LEN 32
#define SC_NONCE_LEN 12
#define SC_HMAC_LEN 32
#define SC_ENC2_PREFIX "enc2:"
#define SC_ENC2_PREFIX_LEN 5

struct sc_secret_store {
    char key_path[1024];
    size_t key_path_len;
    bool enabled;
};

static int dev_urandom_bytes(uint8_t *buf, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return (n == len) ? 0 : -1;
}

void sc_hex_encode(const uint8_t *data, size_t len, char *out_hex) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out_hex[i * 2] = hex[data[i] >> 4];
        out_hex[i * 2 + 1] = hex[data[i] & 0x0f];
    }
    out_hex[len * 2] = '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

sc_error_t sc_hex_decode(const char *hex, size_t hex_len,
                         uint8_t *out_data, size_t out_cap, size_t *out_len) {
    if (hex_len & 1) return SC_ERR_PARSE;
    size_t byte_len = hex_len / 2;
    if (out_cap < byte_len) return SC_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < byte_len; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return SC_ERR_PARSE;
        out_data[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = byte_len;
    return SC_OK;
}

bool sc_secret_store_is_encrypted(const char *value) {
    return value && strlen(value) >= SC_ENC2_PREFIX_LEN &&
           memcmp(value, SC_ENC2_PREFIX, SC_ENC2_PREFIX_LEN) == 0;
}

static sc_error_t load_or_create_key(sc_secret_store_t *store, uint8_t key[SC_KEY_LEN]) {
    const char *path = store->key_path;

    FILE *f = fopen(path, "rb");
    if (f) {
        char hex_buf[80];
        size_t n = fread(hex_buf, 1, sizeof(hex_buf) - 1, f);
        fclose(f);
        hex_buf[n] = '\0';
        /* trim */
        while (n > 0 && (hex_buf[n-1] == ' ' || hex_buf[n-1] == '\t' ||
                         hex_buf[n-1] == '\n' || hex_buf[n-1] == '\r'))
            n--;
        hex_buf[n] = '\0';
        size_t dlen;
        sc_error_t err = sc_hex_decode(hex_buf, strlen(hex_buf), key, SC_KEY_LEN, &dlen);
        if (err != SC_OK || dlen != SC_KEY_LEN) return SC_ERR_CRYPTO_DECRYPT;
        return SC_OK;
    }

    /* Create new key */
    if (dev_urandom_bytes(key, SC_KEY_LEN) != 0)
        return SC_ERR_CRYPTO_ENCRYPT;

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

    f = fopen(path, "wb");
    if (!f) return SC_ERR_IO;
    char hex_out[SC_KEY_LEN * 2 + 1];
    sc_hex_encode(key, SC_KEY_LEN, hex_out);
    fwrite(hex_out, 1, SC_KEY_LEN * 2, f);
    fclose(f);
    return SC_OK;
}

sc_secret_store_t *sc_secret_store_create(sc_allocator_t *alloc, const char *config_dir, bool enabled) {
    if (!alloc || !config_dir) return NULL;

    sc_secret_store_t *store = (sc_secret_store_t *)alloc->alloc(alloc->ctx, sizeof(sc_secret_store_t));
    if (!store) return NULL;
    memset(store, 0, sizeof(*store));
    store->enabled = enabled;

    int n = snprintf(store->key_path, sizeof(store->key_path), "%s/.secret_key", config_dir);
    if (n <= 0 || (size_t)n >= sizeof(store->key_path)) {
        store->key_path[0] = '\0';
    }
    store->key_path_len = strlen(store->key_path);

    return store;
}

void sc_secret_store_destroy(sc_secret_store_t *store, sc_allocator_t *alloc) {
    if (!store || !alloc) return;
    alloc->free(alloc->ctx, store, sizeof(sc_secret_store_t));
}

sc_error_t sc_secret_store_encrypt(sc_secret_store_t *store,
                                   sc_allocator_t *alloc,
                                   const char *plaintext,
                                   char **out_ciphertext_hex) {
    if (!store || !alloc || !out_ciphertext_hex) return SC_ERR_INVALID_ARGUMENT;
    *out_ciphertext_hex = NULL;

    if (!store->enabled || !plaintext || !plaintext[0]) {
        size_t len = plaintext ? strlen(plaintext) : 0;
        char *dup = (char *)alloc->alloc(alloc->ctx, len + 1);
        if (!dup) return SC_ERR_OUT_OF_MEMORY;
        if (plaintext) memcpy(dup, plaintext, len + 1);
        else dup[0] = '\0';
        *out_ciphertext_hex = dup;
        return SC_OK;
    }

    uint8_t key[SC_KEY_LEN];
    sc_error_t err = load_or_create_key(store, key);
    if (err != SC_OK) return err;

    uint8_t nonce[SC_NONCE_LEN];
    if (dev_urandom_bytes(nonce, SC_NONCE_LEN) != 0)
        return SC_ERR_CRYPTO_ENCRYPT;

    size_t plen = strlen(plaintext);
    uint8_t *ciphertext = (uint8_t *)alloc->alloc(alloc->ctx, plen + SC_HMAC_LEN);
    if (!ciphertext) return SC_ERR_OUT_OF_MEMORY;

    sc_chacha20_encrypt(key, nonce, 1, (const uint8_t *)plaintext, ciphertext, plen);

    uint8_t hmac[32];
    sc_hmac_sha256(key, SC_KEY_LEN, ciphertext, plen, hmac);

    size_t blob_len = SC_NONCE_LEN + plen + SC_HMAC_LEN;
    uint8_t *blob = (uint8_t *)alloc->alloc(alloc->ctx, blob_len);
    if (!blob) {
        alloc->free(alloc->ctx, ciphertext, plen + SC_HMAC_LEN);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(blob, nonce, SC_NONCE_LEN);
    memcpy(blob + SC_NONCE_LEN, ciphertext, plen);
    memcpy(blob + SC_NONCE_LEN + plen, hmac, SC_HMAC_LEN);
    alloc->free(alloc->ctx, ciphertext, plen + SC_HMAC_LEN);

    size_t hex_len = blob_len * 2 + SC_ENC2_PREFIX_LEN + 1;
    char *hex_out = (char *)alloc->alloc(alloc->ctx, hex_len);
    if (!hex_out) {
        alloc->free(alloc->ctx, blob, blob_len);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(hex_out, SC_ENC2_PREFIX, SC_ENC2_PREFIX_LEN);
    sc_hex_encode(blob, blob_len, hex_out + SC_ENC2_PREFIX_LEN);
    alloc->free(alloc->ctx, blob, blob_len);

    *out_ciphertext_hex = hex_out;
    return SC_OK;
}

sc_error_t sc_secret_store_decrypt(sc_secret_store_t *store,
                                   sc_allocator_t *alloc,
                                   const char *value,
                                   char **out_plaintext) {
    if (!store || !alloc || !out_plaintext) return SC_ERR_INVALID_ARGUMENT;
    *out_plaintext = NULL;

    if (!value) return SC_ERR_INVALID_ARGUMENT;

    if (!sc_secret_store_is_encrypted(value)) {
        char *dup = (char *)alloc->alloc(alloc->ctx, strlen(value) + 1);
        if (!dup) return SC_ERR_OUT_OF_MEMORY;
        memcpy(dup, value, strlen(value) + 1);
        *out_plaintext = dup;
        return SC_OK;
    }

    const char *hex_str = value + SC_ENC2_PREFIX_LEN;
    size_t hex_len = strlen(hex_str);
    if (hex_len & 1) return SC_ERR_CRYPTO_DECRYPT;

    size_t blob_len = hex_len / 2;
    if (blob_len <= SC_NONCE_LEN) return SC_ERR_CRYPTO_DECRYPT;

    uint8_t blob[8192];
    if (blob_len > sizeof(blob)) return SC_ERR_CRYPTO_DECRYPT;

    size_t decoded;
    sc_error_t err = sc_hex_decode(hex_str, hex_len, blob, sizeof(blob), &decoded);
    if (err != SC_OK || decoded != blob_len) return SC_ERR_CRYPTO_DECRYPT;

    uint8_t key[SC_KEY_LEN];
    err = load_or_create_key(store, key);
    if (err != SC_OK) return err;

    size_t ct_len = blob_len - SC_NONCE_LEN - SC_HMAC_LEN;
    uint8_t computed_hmac[32];
    sc_hmac_sha256(key, SC_KEY_LEN, blob + SC_NONCE_LEN, ct_len, computed_hmac);

    /* Constant-time compare */
    unsigned char diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= computed_hmac[i] ^ blob[SC_NONCE_LEN + ct_len + i];
    if (diff != 0) return SC_ERR_CRYPTO_DECRYPT;

    uint8_t plain_buf[8192];
    if (ct_len > sizeof(plain_buf)) return SC_ERR_CRYPTO_DECRYPT;

    sc_chacha20_decrypt(key, blob, 1, blob + SC_NONCE_LEN, plain_buf, ct_len);

    char *plain = (char *)alloc->alloc(alloc->ctx, ct_len + 1);
    if (!plain) return SC_ERR_OUT_OF_MEMORY;
    memcpy(plain, plain_buf, ct_len);
    plain[ct_len] = '\0';
    *out_plaintext = plain;
    return SC_OK;
}
