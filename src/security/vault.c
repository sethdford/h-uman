/*
 * Secure secrets vault — key-value store with XOR/base64 obfuscation.
 * Uses HUMAN_VAULT_KEY env var for XOR key; falls back to base64 (obfuscation only).
 * HU_IS_TEST: in-memory storage only, no file I/O.
 */
#include "human/security/vault.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/stat.h>
#endif

#if defined(__STDC_LIB_EXT1__)
#define hu_vault_secure_zero(p, n) memset_s((p), (n), 0, (n))
#elif defined(__GNUC__) || defined(__clang__)
static void hu_vault_secure_zero(void *p, size_t n) {
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");
}
#else
static void hu_vault_secure_zero(void *p, size_t n) {
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--)
        *vp++ = 0;
}
#endif

#define VAULT_KEY_MAX  64
#define VAULT_PATH_MAX 1024

struct hu_vault {
    hu_allocator_t *alloc;
    char vault_path[VAULT_PATH_MAX];
    unsigned char key[VAULT_KEY_MAX];
    size_t key_len;
    bool has_key;
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_json_value_t *in_memory;
#endif
};

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_cap) {
    size_t out_size = ((in_len + 2) / 3) * 4;
    if (out_cap < out_size + 1)
        return 0;
    size_t j = 0;
    for (size_t t = 0; t < in_len / 3; t++) {
        uint32_t val = ((uint32_t)in[t * 3] << 16) | ((uint32_t)in[t * 3 + 1] << 8) | in[t * 3 + 2];
        out[j++] = b64_table[(val >> 18) & 0x3F];
        out[j++] = b64_table[(val >> 12) & 0x3F];
        out[j++] = b64_table[(val >> 6) & 0x3F];
        out[j++] = b64_table[val & 0x3F];
    }
    size_t rem = in_len % 3;
    if (rem == 1) {
        uint32_t val = (uint32_t)in[in_len - 1] << 16;
        out[j++] = b64_table[(val >> 18) & 0x3F];
        out[j++] = b64_table[(val >> 12) & 0x3F];
        out[j++] = '=';
        out[j++] = '=';
    } else if (rem == 2) {
        uint32_t val = ((uint32_t)in[in_len - 2] << 16) | ((uint32_t)in[in_len - 1] << 8);
        out[j++] = b64_table[(val >> 18) & 0x3F];
        out[j++] = b64_table[(val >> 12) & 0x3F];
        out[j++] = b64_table[(val >> 6) & 0x3F];
        out[j++] = '=';
    }
    out[j] = '\0';
    return j;
}

static int b64_char_val(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static hu_error_t base64_decode(const char *in, size_t in_len, unsigned char *out, size_t out_cap,
                                size_t *out_len) {
    while (in_len > 0 && in[in_len - 1] == '=')
        in_len--;
    size_t byte_len = (in_len * 3) / 4;
    if (out_cap < byte_len)
        return HU_ERR_INVALID_ARGUMENT;
    size_t j = 0;
    for (size_t i = 0; i + 4 <= in_len; i += 4) {
        int a = b64_char_val(in[i]);
        int b = b64_char_val(in[i + 1]);
        int c = b64_char_val(in[i + 2]);
        int d = b64_char_val(in[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0)
            return HU_ERR_PARSE;
        uint32_t val = (uint32_t)(a << 18) | (b << 12) | (c << 6) | d;
        out[j++] = (unsigned char)(val >> 16);
        out[j++] = (unsigned char)(val >> 8);
        out[j++] = (unsigned char)val;
    }
    if (in_len % 4 == 2) {
        int a = b64_char_val(in[in_len - 2]);
        int b = b64_char_val(in[in_len - 1]);
        if (a < 0 || b < 0)
            return HU_ERR_PARSE;
        uint32_t val = (uint32_t)(a << 18) | (b << 12);
        out[j++] = (unsigned char)(val >> 16);
    } else if (in_len % 4 == 3) {
        int a = b64_char_val(in[in_len - 3]);
        int b = b64_char_val(in[in_len - 2]);
        int c = b64_char_val(in[in_len - 1]);
        if (a < 0 || b < 0 || c < 0)
            return HU_ERR_PARSE;
        uint32_t val = (uint32_t)(a << 18) | (b << 12) | (c << 6);
        out[j++] = (unsigned char)(val >> 16);
        out[j++] = (unsigned char)(val >> 8);
    }
    *out_len = j;
    return HU_OK;
}

static void xor_crypt(const unsigned char *key, size_t key_len, const unsigned char *in,
                      size_t in_len, unsigned char *out) {
    for (size_t i = 0; i < in_len; i++)
        out[i] = in[i] ^ key[i % key_len];
}

static void derive_key(const char *env_val, unsigned char *key_out, size_t *key_len_out) {
    size_t len = env_val ? strlen(env_val) : 0;
    if (len > VAULT_KEY_MAX)
        len = VAULT_KEY_MAX;
    if (len == 0) {
        *key_len_out = 0;
        return;
    }
    memcpy(key_out, env_val, len);
    *key_len_out = len;
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static hu_error_t ensure_parent_dir(const char *path) {
    char *slash = strrchr(path, '/');
    if (!slash || slash <= path)
        return HU_OK;
    char dir[VAULT_PATH_MAX];
    size_t dlen = (size_t)(slash - path);
    if (dlen >= sizeof(dir))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(dir, path, dlen);
    dir[dlen] = '\0';
#ifdef _WIN32
    (void)dir;
    (void)dlen;
    return HU_OK;
#else
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return HU_ERR_IO;
    return HU_OK;
#endif
}

static hu_error_t vault_load(hu_vault_t *v, hu_json_value_t **out) {
    FILE *f = fopen(v->vault_path, "rb");
    if (!f) {
        *out = hu_json_object_new(v->alloc);
        return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        *out = hu_json_object_new(v->alloc);
        return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }
    char *buf = (char *)v->alloc->alloc(v->alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    hu_error_t err = hu_json_parse(v->alloc, buf, n, out);
    v->alloc->free(v->alloc->ctx, buf, (size_t)sz + 1);
    if (err != HU_OK) {
        *out = hu_json_object_new(v->alloc);
        return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }
    if (!*out || (*out)->type != HU_JSON_OBJECT) {
        if (*out)
            hu_json_free(v->alloc, *out);
        *out = hu_json_object_new(v->alloc);
        return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }
    return HU_OK;
}

static hu_error_t vault_save(hu_vault_t *v, hu_json_value_t *obj) {
    char *json = NULL;
    size_t json_len = 0;
    hu_error_t err = hu_json_stringify(v->alloc, obj, &json, &json_len);
    if (err != HU_OK)
        return err;
    err = ensure_parent_dir(v->vault_path);
    if (err != HU_OK) {
        v->alloc->free(v->alloc->ctx, json, json_len + 1);
        return err;
    }
    FILE *f = fopen(v->vault_path, "wb");
    if (!f) {
        v->alloc->free(v->alloc->ctx, json, json_len + 1);
        return HU_ERR_IO;
    }
    size_t written = fwrite(json, 1, json_len, f);
    fclose(f);
    v->alloc->free(v->alloc->ctx, json, json_len + 1);
    return (written == json_len) ? HU_OK : HU_ERR_IO;
}
#endif

hu_vault_t *hu_vault_create(hu_allocator_t *alloc, const char *vault_path) {
    if (!alloc)
        return NULL;

    hu_vault_t *v = (hu_vault_t *)alloc->alloc(alloc->ctx, sizeof(hu_vault_t));
    if (!v)
        return NULL;
    memset(v, 0, sizeof(*v));
    v->alloc = alloc;

    if (vault_path && vault_path[0]) {
        size_t len = strlen(vault_path);
        if (len >= VAULT_PATH_MAX)
            len = VAULT_PATH_MAX - 1;
        memcpy(v->vault_path, vault_path, len);
        v->vault_path[len] = '\0';
    } else {
        const char *home = getenv("HOME");
        if (!home)
            home = ".";
        int n = snprintf(v->vault_path, sizeof(v->vault_path), "%s/.human/vault.json", home);
        if (n <= 0 || (size_t)n >= sizeof(v->vault_path))
            v->vault_path[0] = '\0';
    }

    const char *key_env = getenv("HUMAN_VAULT_KEY");
    if (key_env && key_env[0]) {
        derive_key(key_env, v->key, &v->key_len);
        v->has_key = (v->key_len > 0);
    } else {
        v->has_key = false;
        v->key_len = 0;
#ifndef HU_IS_TEST
        fprintf(
            stderr,
            "[vault] HUMAN_VAULT_KEY not set — secrets stored as base64 (obfuscation only)\n");
#endif
    }

#if defined(HU_IS_TEST) && HU_IS_TEST
    v->in_memory = hu_json_object_new(alloc);
    if (!v->in_memory) {
        alloc->free(alloc->ctx, v, sizeof(hu_vault_t));
        return NULL;
    }
#endif

    return v;
}

hu_error_t hu_vault_set(hu_vault_t *vault, const char *key, const char *value) {
    if (!vault || !key || !value)
        return HU_ERR_INVALID_ARGUMENT;

    size_t val_len = strlen(value);
    hu_json_value_t *val_json = NULL;

    if (vault->has_key && vault->key_len > 0) {
        unsigned char *enc = (unsigned char *)vault->alloc->alloc(vault->alloc->ctx, val_len);
        if (!enc)
            return HU_ERR_OUT_OF_MEMORY;
        xor_crypt(vault->key, vault->key_len, (const unsigned char *)value, val_len, enc);
        char b64[4096];
        size_t b64_len = base64_encode(enc, val_len, b64, sizeof(b64));
        vault->alloc->free(vault->alloc->ctx, enc, val_len);
        if (b64_len == 0)
            return HU_ERR_INVALID_ARGUMENT;
        val_json = hu_json_string_new(vault->alloc, b64, b64_len);
    } else {
        char b64[4096];
        size_t b64_len = base64_encode((const unsigned char *)value, val_len, b64, sizeof(b64));
        if (b64_len == 0)
            return HU_ERR_INVALID_ARGUMENT;
        val_json = hu_json_string_new(vault->alloc, b64, b64_len);
    }
    if (!val_json)
        return HU_ERR_OUT_OF_MEMORY;

#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_json_object_set(vault->alloc, vault->in_memory, key, val_json);
    return HU_OK;
#else
    hu_json_value_t *obj = NULL;
    hu_error_t err = vault_load(vault, &obj);
    if (err != HU_OK)
        return err;
    hu_json_object_set(vault->alloc, obj, key, val_json);
    err = vault_save(vault, obj);
    hu_json_free(vault->alloc, obj);
    return err;
#endif
}

hu_error_t hu_vault_get(hu_vault_t *vault, const char *key, char *out, size_t out_size) {
    if (!vault || !key || !out || out_size == 0)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_json_value_t *val = hu_json_object_get(vault->in_memory, key);
#else
    hu_json_value_t *obj = NULL;
    hu_error_t err = vault_load(vault, &obj);
    if (err != HU_OK)
        return err;
    hu_json_value_t *val = hu_json_object_get(obj, key);
#endif

    if (!val || val->type != HU_JSON_STRING) {
#if !(defined(HU_IS_TEST) && HU_IS_TEST)
        hu_json_free(vault->alloc, obj);
#endif
        return HU_ERR_NOT_FOUND;
    }

    const char *stored = val->data.string.ptr;
    size_t stored_len = val->data.string.len;

    unsigned char decoded[4096];
    size_t decoded_len = 0;
    hu_error_t decode_err =
        base64_decode(stored, stored_len, decoded, sizeof(decoded), &decoded_len);
#if !(defined(HU_IS_TEST) && HU_IS_TEST)
    hu_json_free(vault->alloc, obj);
#endif
    if (decode_err != HU_OK)
        return decode_err;
    if (decoded_len >= out_size) {
        hu_vault_secure_zero(decoded, decoded_len);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (vault->has_key && vault->key_len > 0) {
        xor_crypt(vault->key, vault->key_len, decoded, decoded_len, decoded);
    }
    memcpy(out, decoded, decoded_len);
    out[decoded_len] = '\0';
    hu_vault_secure_zero(decoded, decoded_len);
    return HU_OK;
}

hu_error_t hu_vault_delete(hu_vault_t *vault, const char *key) {
    if (!vault || !key)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    if (!hu_json_object_remove(vault->alloc, vault->in_memory, key))
        return HU_ERR_NOT_FOUND;
    return HU_OK;
#else
    hu_json_value_t *obj = NULL;
    hu_error_t err = vault_load(vault, &obj);
    if (err != HU_OK)
        return err;
    if (!hu_json_object_remove(vault->alloc, obj, key)) {
        hu_json_free(vault->alloc, obj);
        return HU_ERR_NOT_FOUND;
    }
    err = vault_save(vault, obj);
    hu_json_free(vault->alloc, obj);
    return err;
#endif
}

hu_error_t hu_vault_list_keys(hu_vault_t *vault, char **keys, size_t max_keys, size_t *count) {
    if (!vault || !keys || !count)
        return HU_ERR_INVALID_ARGUMENT;
    *count = 0;

#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_json_value_t *obj = vault->in_memory;
#else
    hu_json_value_t *obj = NULL;
    hu_error_t err = vault_load(vault, &obj);
    if (err != HU_OK)
        return err;
#endif

    size_t n = 0;
    for (size_t i = 0; i < obj->data.object.len && n < max_keys; i++) {
        hu_json_pair_t *pair = &obj->data.object.pairs[i];
        if (pair->key && pair->key_len > 0) {
            keys[n] = hu_strndup(vault->alloc, pair->key, pair->key_len);
            if (!keys[n]) {
#if !(defined(HU_IS_TEST) && HU_IS_TEST)
                for (size_t j = 0; j < n; j++)
                    vault->alloc->free(vault->alloc->ctx, keys[j], strlen(keys[j]) + 1);
                hu_json_free(vault->alloc, obj);
#endif
                return HU_ERR_OUT_OF_MEMORY;
            }
            n++;
        }
    }
    *count = n;

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
    hu_json_free(vault->alloc, obj);
#endif
    return HU_OK;
}

void hu_vault_destroy(hu_vault_t *vault) {
    if (!vault)
        return;
    hu_vault_secure_zero(vault->key, sizeof(vault->key));
#if defined(HU_IS_TEST) && HU_IS_TEST
    if (vault->in_memory)
        hu_json_free(vault->alloc, vault->in_memory);
#endif
    vault->alloc->free(vault->alloc->ctx, vault, sizeof(hu_vault_t));
}

static void env_key_for_provider(const char *provider, char *buf, size_t buf_size) {
    if (buf_size == 0)
        return;
    size_t i = 0;
    for (; provider[i] && i + 9 < buf_size; i++)
        buf[i] = (char)(unsigned char)toupper((unsigned char)provider[i]);
    buf[i] = '\0';
    if (i + 9 <= buf_size)
        snprintf(buf + i, buf_size - i, "_API_KEY");
}

hu_error_t hu_vault_get_api_key(hu_vault_t *vault, hu_allocator_t *alloc, const char *provider_name,
                                const char *config_api_key, char **out) {
    if (!alloc || !provider_name || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    char vault_key[128];
    int n = snprintf(vault_key, sizeof(vault_key), "%s_api_key", provider_name);
    if (n <= 0 || (size_t)n >= sizeof(vault_key))
        return HU_ERR_INVALID_ARGUMENT;

    if (vault) {
        char tmp[512];
        if (hu_vault_get(vault, vault_key, tmp, sizeof(tmp)) == HU_OK) {
            *out = hu_strdup(alloc, tmp);
            hu_vault_secure_zero(tmp, sizeof(tmp));
            return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
        }
    }

    char env_name[64];
    env_key_for_provider(provider_name, env_name, sizeof(env_name));
    const char *env_val = getenv(env_name);
    if (env_val && env_val[0]) {
        *out = hu_strdup(alloc, env_val);
        return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }

    if (config_api_key && config_api_key[0]) {
        *out = hu_strdup(alloc, config_api_key);
        return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }

    return HU_ERR_NOT_FOUND;
}
