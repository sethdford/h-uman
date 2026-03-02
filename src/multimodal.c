#include "seaclaw/multimodal.h"
#include "seaclaw/core/allocator.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#if !defined(_WIN32) && !defined(_WIN64) && !defined(__CYGWIN__)
#include <unistd.h>
#include <sys/stat.h>
#endif

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int to_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool match_marker(const char *s, size_t len, const char *marker, size_t marker_len) {
    if (len < marker_len) return false;
    for (size_t i = 0; i < marker_len; i++) {
        if (to_lower((unsigned char)s[i]) != (unsigned char)marker[i]) return false;
    }
    return true;
}

sc_error_t sc_multimodal_encode_base64(sc_allocator_t *alloc,
    const void *data, size_t data_len,
    char **out_base64, size_t *out_len)
{
    if (!alloc || !out_base64 || !out_len) return SC_ERR_INVALID_ARGUMENT;
    *out_base64 = NULL;
    *out_len = 0;

    size_t out_size = ((data_len + 2) / 3) * 4;
    char *buf = (char *)alloc->alloc(alloc->ctx, out_size + 1);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;

    const unsigned char *src = (const unsigned char *)data;
    size_t j = 0;
    size_t full_triples = data_len / 3;
    size_t remaining = data_len % 3;

    for (size_t t = 0; t < full_triples; t++) {
        uint32_t val = ((uint32_t)src[t * 3] << 16) | ((uint32_t)src[t * 3 + 1] << 8) | src[t * 3 + 2];
        buf[j++] = b64_table[(val >> 18) & 0x3F];
        buf[j++] = b64_table[(val >> 12) & 0x3F];
        buf[j++] = b64_table[(val >> 6) & 0x3F];
        buf[j++] = b64_table[val & 0x3F];
    }
    if (remaining == 1) {
        uint32_t val = (uint32_t)src[full_triples * 3] << 16;
        buf[j++] = b64_table[(val >> 18) & 0x3F];
        buf[j++] = b64_table[(val >> 12) & 0x3F];
        buf[j++] = '=';
        buf[j++] = '=';
    } else if (remaining == 2) {
        uint32_t val = ((uint32_t)src[full_triples * 3] << 16) | ((uint32_t)src[full_triples * 3 + 1] << 8);
        buf[j++] = b64_table[(val >> 18) & 0x3F];
        buf[j++] = b64_table[(val >> 12) & 0x3F];
        buf[j++] = b64_table[(val >> 6) & 0x3F];
        buf[j++] = '=';
    }
    buf[j] = '\0';
    *out_base64 = buf;
    *out_len = j;
    return SC_OK;
}

const char *sc_multimodal_detect_mime(const void *header, size_t header_len) {
    const unsigned char *h = (const unsigned char *)header;
    if (header_len >= 4 && h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') return "image/png";
    if (header_len >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return "image/jpeg";
    if (header_len >= 4 && h[0] == 'G' && h[1] == 'I' && h[2] == 'F' && h[3] == '8') return "image/gif";
    if (header_len >= 12 && h[0] == 'R' && h[1] == 'I' && h[2] == 'F' && h[3] == 'F' &&
        h[8] == 'W' && h[9] == 'E' && h[10] == 'B' && h[11] == 'P') return "image/webp";
    if (header_len >= 2 && h[0] == 'B' && h[1] == 'M') return "image/bmp";
    return "application/octet-stream";
}

sc_error_t sc_multimodal_encode_image(sc_allocator_t *alloc,
    const char *file_path,
    char **out_data_uri, size_t *out_len)
{
    if (!alloc || !file_path || !out_data_uri || !out_len) return SC_ERR_INVALID_ARGUMENT;
    *out_data_uri = NULL;
    *out_len = 0;

#if defined(SC_IS_TEST) && SC_IS_TEST
    (void)file_path;
    const char *mock = "data:image/png;base64,iVBORw0KGgo=";
    size_t len = strlen(mock);
    char *out = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!out) return SC_ERR_OUT_OF_MEMORY;
    memcpy(out, mock, len + 1);
    *out_data_uri = out;
    *out_len = len;
    return SC_OK;
#else
#if !defined(_WIN32) && !defined(_WIN64) && !defined(__CYGWIN__)
    struct stat st;
    if (stat(file_path, &st) != 0) return SC_ERR_NOT_FOUND;
    if (!S_ISREG(st.st_mode)) return SC_ERR_INVALID_ARGUMENT;
    if ((size_t)st.st_size > SC_MULTIMODAL_MAX_IMAGE_SIZE) return SC_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(file_path, "rb");
    if (!f) return SC_ERR_IO;

    size_t file_size = (size_t)st.st_size;
    unsigned char *buf = (unsigned char *)alloc->alloc(alloc->ctx, file_size);
    if (!buf) {
        fclose(f);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t nr = fread(buf, 1, file_size, f);
    fclose(f);
    if (nr != file_size) {
        alloc->free(alloc->ctx, buf, file_size);
        return SC_ERR_IO;
    }

    const char *mime = sc_multimodal_detect_mime(buf, file_size);

    char *b64 = NULL;
    size_t b64_len = 0;
    sc_error_t err = sc_multimodal_encode_base64(alloc, buf, file_size, &b64, &b64_len);
    alloc->free(alloc->ctx, buf, file_size);
    if (err != SC_OK) return err;

    size_t prefix_len = strlen("data:") + strlen(mime) + strlen(";base64,");
    size_t total = prefix_len + b64_len + 1;
    char *data_uri = (char *)alloc->alloc(alloc->ctx, total);
    if (!data_uri) {
        alloc->free(alloc->ctx, b64, b64_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(data_uri, total, "data:%s;base64,%s", mime, b64);
    alloc->free(alloc->ctx, b64, b64_len + 1);
    if (n < 0 || (size_t)n >= total) {
        alloc->free(alloc->ctx, data_uri, total);
        return SC_ERR_INTERNAL;
    }
    *out_data_uri = data_uri;
    *out_len = (size_t)n;
    return SC_OK;
#else
    (void)file_path;
    return SC_ERR_NOT_SUPPORTED;
#endif
#endif
}

sc_error_t sc_multimodal_encode_image_raw(const void *img_data, size_t len, char **out_base64) {
    if (!img_data || !out_base64) return SC_ERR_INVALID_ARGUMENT;
    sc_allocator_t alloc = sc_system_allocator();
    size_t out_len = 0;
    sc_error_t err = sc_multimodal_encode_base64(&alloc, img_data, len, out_base64, &out_len);
    return err;
}

static sc_image_ref_type_t classify_ref(const char *val, size_t len) {
    if (len >= 5 && to_lower(val[0]) == 'd' && to_lower(val[1]) == 'a' && to_lower(val[2]) == 't' &&
        to_lower(val[3]) == 'a' && val[4] == ':')
        return SC_IMAGE_REF_DATA_URI;
    if (len >= 8 && val[0] == 'h' && val[1] == 't' && val[2] == 't' && val[3] == 'p' &&
        val[4] == 's' && val[5] == ':' && val[6] == '/' && val[7] == '/')
        return SC_IMAGE_REF_URL;
    if (len >= 7 && val[0] == 'h' && val[1] == 't' && val[2] == 't' && val[3] == 'p' &&
        val[4] == ':' && val[5] == '/' && val[6] == '/')
        return SC_IMAGE_REF_URL;
    return SC_IMAGE_REF_LOCAL;
}

static bool is_image_kind(const char *s, size_t len) {
    if (len == 5 && match_marker(s, len, "image", 5)) return true;
    if (len == 5 && match_marker(s, len, "photo", 5)) return true;
    if (len == 3 && match_marker(s, len, "img", 3)) return true;
    return false;
}

sc_error_t sc_multimodal_parse_markers(sc_allocator_t *alloc,
    const char *text, size_t text_len,
    sc_image_ref_t **out_refs, size_t *out_ref_count,
    char **out_cleaned_text, size_t *out_cleaned_len)
{
    if (!alloc || !text || !out_refs || !out_ref_count || !out_cleaned_text || !out_cleaned_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out_refs = NULL;
    *out_ref_count = 0;
    *out_cleaned_text = NULL;
    *out_cleaned_len = 0;

    /* Simple two-pass: first count refs and measure cleaned size, then allocate and fill */
    size_t ref_cap = 4;
    sc_image_ref_t *refs = (sc_image_ref_t *)alloc->alloc(alloc->ctx, ref_cap * sizeof(sc_image_ref_t));
    if (!refs) return SC_ERR_OUT_OF_MEMORY;
    size_t ref_count = 0;

    size_t cleaned_cap = text_len + 1;
    char *cleaned = (char *)alloc->alloc(alloc->ctx, cleaned_cap);
    if (!cleaned) {
        alloc->free(alloc->ctx, refs, ref_cap * sizeof(sc_image_ref_t));
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t cleaned_pos = 0;

    size_t i = 0;
    while (i < text_len) {
        if (text[i] != '[') {
            if (cleaned_pos + 1 >= cleaned_cap) {
                size_t new_cap = cleaned_cap * 2;
                char *n = (char *)alloc->realloc(alloc->ctx, cleaned, cleaned_cap, new_cap);
                if (!n) {
                    for (size_t r = 0; r < ref_count; r++)
                        alloc->free(alloc->ctx, (void *)refs[r].value, refs[r].value_len + 1);
                    alloc->free(alloc->ctx, refs, ref_cap * sizeof(sc_image_ref_t));
                    alloc->free(alloc->ctx, cleaned, cleaned_cap);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                cleaned = n;
                cleaned_cap = new_cap;
            }
            cleaned[cleaned_pos++] = text[i++];
            continue;
        }

        size_t close_pos = i + 1;
        while (close_pos < text_len && text[close_pos] != ']') close_pos++;
        if (close_pos >= text_len) {
            cleaned[cleaned_pos++] = text[i++];
            continue;
        }

        const char *marker = text + i + 1;
        size_t marker_len = close_pos - (i + 1);
        const char *colon = memchr(marker, ':', marker_len);
        if (!colon) {
            for (size_t k = i; k <= close_pos; k++) {
                if (cleaned_pos + 1 >= cleaned_cap) {
                    size_t new_cap = cleaned_cap * 2;
                    char *n = (char *)alloc->realloc(alloc->ctx, cleaned, cleaned_cap, new_cap);
                    if (!n) goto parse_fail;
                    cleaned = n;
                    cleaned_cap = new_cap;
                }
                cleaned[cleaned_pos++] = text[k];
            }
            i = close_pos + 1;
            continue;
        }

        size_t kind_len = (size_t)(colon - marker);
        const char *val_start = colon + 1;
        size_t val_len = marker_len - kind_len - 1;
        while (val_len > 0 && (val_start[0] == ' ' || val_start[0] == '\t')) { val_start++; val_len--; }
        while (val_len > 0 && (val_start[val_len - 1] == ' ' || val_start[val_len - 1] == '\t')) val_len--;

        if (val_len > 0 && is_image_kind(marker, kind_len)) {
            if (ref_count >= ref_cap) {
                size_t new_ref_cap = ref_cap * 2;
                sc_image_ref_t *nr = (sc_image_ref_t *)alloc->realloc(alloc->ctx, refs, ref_cap * sizeof(sc_image_ref_t), new_ref_cap * sizeof(sc_image_ref_t));
                if (!nr) goto parse_fail;
                refs = nr;
                ref_cap = new_ref_cap;
            }
            char *val_copy = (char *)alloc->alloc(alloc->ctx, val_len + 1);
            if (!val_copy) goto parse_fail;
            memcpy(val_copy, val_start, val_len);
            val_copy[val_len] = '\0';
            refs[ref_count].type = classify_ref(val_copy, val_len);
            refs[ref_count].value = val_copy;
            refs[ref_count].value_len = val_len;
            ref_count++;
            i = close_pos + 1;
            continue;
        }

        for (size_t k = i; k <= close_pos; k++) {
            if (cleaned_pos + 1 >= cleaned_cap) {
                size_t new_cap = cleaned_cap * 2;
                char *n = (char *)alloc->realloc(alloc->ctx, cleaned, cleaned_cap, new_cap);
                if (!n) goto parse_fail;
                cleaned = n;
                cleaned_cap = new_cap;
            }
            cleaned[cleaned_pos++] = text[k];
        }
        i = close_pos + 1;
    }
    cleaned[cleaned_pos] = '\0';
    *out_refs = refs;
    *out_ref_count = ref_count;
    *out_cleaned_text = cleaned;
    *out_cleaned_len = cleaned_pos;
    return SC_OK;

parse_fail:
    for (size_t r = 0; r < ref_count; r++)
        alloc->free(alloc->ctx, (void *)refs[r].value, refs[r].value_len + 1);
    alloc->free(alloc->ctx, refs, ref_cap * sizeof(sc_image_ref_t));
    alloc->free(alloc->ctx, cleaned, cleaned_cap);
    return SC_ERR_OUT_OF_MEMORY;
}

sc_error_t sc_multimodal_build_openai_image(sc_allocator_t *alloc,
    const char *data_uri, size_t data_uri_len,
    char **out_json, size_t *out_json_len)
{
    if (!alloc || !data_uri || !out_json || !out_json_len) return SC_ERR_INVALID_ARGUMENT;
    size_t need = 64 + data_uri_len * 2;
    char *buf = (char *)alloc->alloc(alloc->ctx, need);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
    int n = snprintf(buf, need, "{\"type\":\"image_url\",\"image_url\":{\"url\":\"%.*s\"}}", (int)data_uri_len, data_uri);
    if (n < 0 || (size_t)n >= need) {
        alloc->free(alloc->ctx, buf, need);
        return SC_ERR_INTERNAL;
    }
    *out_json = buf;
    *out_json_len = (size_t)n;
    return SC_OK;
}

sc_error_t sc_multimodal_build_anthropic_image(sc_allocator_t *alloc,
    const char *mime_type, const char *base64_data, size_t base64_len,
    char **out_json, size_t *out_json_len)
{
    if (!alloc || !mime_type || !base64_data || !out_json || !out_json_len) return SC_ERR_INVALID_ARGUMENT;
    size_t need = 80 + strlen(mime_type) + base64_len * 2;
    char *buf = (char *)alloc->alloc(alloc->ctx, need);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
    int n = snprintf(buf, need, "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"%s\",\"data\":\"%.*s\"}}",
        mime_type, (int)base64_len, base64_data);
    if (n < 0 || (size_t)n >= need) {
        alloc->free(alloc->ctx, buf, need);
        return SC_ERR_INTERNAL;
    }
    *out_json = buf;
    *out_json_len = (size_t)n;
    return SC_OK;
}

sc_error_t sc_multimodal_build_gemini_image(sc_allocator_t *alloc,
    const char *mime_type, const char *base64_data, size_t base64_len,
    char **out_json, size_t *out_json_len)
{
    if (!alloc || !mime_type || !base64_data || !out_json || !out_json_len) return SC_ERR_INVALID_ARGUMENT;
    size_t need = 64 + strlen(mime_type) + base64_len * 2;
    char *buf = (char *)alloc->alloc(alloc->ctx, need);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
    int n = snprintf(buf, need, "{\"inlineData\":{\"mimeType\":\"%s\",\"data\":\"%.*s\"}}",
        mime_type, (int)base64_len, base64_data);
    if (n < 0 || (size_t)n >= need) {
        alloc->free(alloc->ctx, buf, need);
        return SC_ERR_INTERNAL;
    }
    *out_json = buf;
    *out_json_len = (size_t)n;
    return SC_OK;
}
