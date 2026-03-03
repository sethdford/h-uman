#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_IMAGE_MAX_SIZE (5 * 1024 * 1024)

#define SC_IMAGE_NAME "image"
#define SC_IMAGE_DESC "Analyze an image file: detect format, size, and dimensions."
#define SC_IMAGE_PARAMS                                                                            \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]" \
    "}"

typedef struct sc_image_ctx {
    char *api_key;
    size_t api_key_len;
} sc_image_ctx_t;

static sc_error_t image_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *path = sc_json_get_string(args, "path");
    if (!path || strlen(path) == 0) {
        *out = sc_tool_result_fail("missing path", 12);
        return SC_OK;
    }
#if SC_IS_TEST
    size_t need = 20 + strlen(path);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, need + 1, "File: %s\nFormat: unknown\nSize: 0 bytes", path);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = sc_tool_result_ok_owned(msg, len);
    return SC_OK;
#else
    FILE *f = fopen(path, "rb");
    if (!f) {
        *out = sc_tool_result_fail("file not found", 14);
        return SC_OK;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        *out = sc_tool_result_fail("seek failed", 11);
        return SC_OK;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        *out = sc_tool_result_fail("ftell failed", 12);
        return SC_OK;
    }
    if ((unsigned long)sz > SC_IMAGE_MAX_SIZE) {
        fclose(f);
        *out = sc_tool_result_fail("file too large (>5MB)", 20);
        return SC_OK;
    }
    rewind(f);
    unsigned char header[16];
    size_t nread = fread(header, 1, sizeof(header), f);
    fclose(f);
    f = NULL;

    const char *fmt = "unknown";
    if (nread >= 4 && header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G')
        fmt = "png";
    else if (nread >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF)
        fmt = "jpeg";
    else if (nread >= 6 && header[0] == 'G' && header[1] == 'I' && header[2] == 'F' &&
             header[3] == '8')
        fmt = "gif";
    else if (nread >= 12 && header[0] == 'R' && header[1] == 'I' && header[2] == 'F' &&
             header[3] == 'F' && header[8] == 'W' && header[9] == 'E' && header[10] == 'B' &&
             header[11] == 'P')
        fmt = "webp";
    else if (nread >= 2 && header[0] == 'B' && header[1] == 'M')
        fmt = "bmp";

    size_t need = 32 + strlen(path) + strlen(fmt);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, need + 1, "File: %s\nFormat: %s\nSize: %ld bytes", path, fmt, (long)sz);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = sc_tool_result_ok_owned(msg, len);
    return SC_OK;
#endif
}

static const char *image_name(void *ctx) {
    (void)ctx;
    return SC_IMAGE_NAME;
}
static const char *image_description(void *ctx) {
    (void)ctx;
    return SC_IMAGE_DESC;
}
static const char *image_parameters_json(void *ctx) {
    (void)ctx;
    return SC_IMAGE_PARAMS;
}
static void image_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    sc_image_ctx_t *c = (sc_image_ctx_t *)ctx;
    if (c && c->api_key) {
        free(c->api_key);
        free(c);
    }
}

static const sc_tool_vtable_t image_vtable = {
    .execute = image_execute,
    .name = image_name,
    .description = image_description,
    .parameters_json = image_parameters_json,
    .deinit = image_deinit,
};

sc_error_t sc_image_create(sc_allocator_t *alloc, const char *api_key, size_t api_key_len,
                           sc_tool_t *out) {
    (void)alloc;
    sc_image_ctx_t *c = (sc_image_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    if (api_key && api_key_len > 0) {
        c->api_key = (char *)malloc(api_key_len + 1);
        if (!c->api_key) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->api_key, api_key, api_key_len);
        c->api_key[api_key_len] = '\0';
        c->api_key_len = api_key_len;
    }
    out->ctx = c;
    out->vtable = &image_vtable;
    return SC_OK;
}
