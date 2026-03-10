#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include "human/tools/validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_IMAGE_MAX_SIZE (5 * 1024 * 1024)

#include "human/tools/schema_common.h"
#define HU_IMAGE_NAME   "image"
#define HU_IMAGE_DESC   "Analyze an image file: detect format, size, and dimensions."
#define HU_IMAGE_PARAMS HU_SCHEMA_PATH_ONLY

typedef struct hu_image_ctx {
    char *api_key;
    size_t api_key_len;
} hu_image_ctx_t;

static hu_error_t image_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                hu_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *path = hu_json_get_string(args, "path");
    if (!path || strlen(path) == 0) {
        *out = hu_tool_result_fail("missing path", 12);
        return HU_OK;
    }
    if (hu_tool_validate_path(path, NULL, 0) != HU_OK) {
        *out = hu_tool_result_fail("path not allowed", 16);
        return HU_OK;
    }
#if HU_IS_TEST
    size_t need = 20 + strlen(path);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, need + 1, "File: %s\nFormat: unknown\nSize: 0 bytes", path);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = hu_tool_result_ok_owned(msg, len);
    return HU_OK;
#else
    FILE *f = fopen(path, "rb");
    if (!f) {
        *out = hu_tool_result_fail("file not found", 14);
        return HU_OK;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        *out = hu_tool_result_fail("seek failed", 11);
        return HU_OK;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        *out = hu_tool_result_fail("ftell failed", 12);
        return HU_OK;
    }
    if ((unsigned long)sz > HU_IMAGE_MAX_SIZE) {
        fclose(f);
        *out = hu_tool_result_fail("file too large (>5MB)", 20);
        return HU_OK;
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
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, need + 1, "File: %s\nFormat: %s\nSize: %ld bytes", path, fmt, (long)sz);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = hu_tool_result_ok_owned(msg, len);
    return HU_OK;
#endif
}

static const char *image_name(void *ctx) {
    (void)ctx;
    return HU_IMAGE_NAME;
}
static const char *image_description(void *ctx) {
    (void)ctx;
    return HU_IMAGE_DESC;
}
static const char *image_parameters_json(void *ctx) {
    (void)ctx;
    return HU_IMAGE_PARAMS;
}
static void image_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_image_ctx_t *c = (hu_image_ctx_t *)ctx;
    if (!c || !alloc)
        return;
    if (c->api_key)
        alloc->free(alloc->ctx, c->api_key, c->api_key_len + 1);
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_tool_vtable_t image_vtable = {
    .execute = image_execute,
    .name = image_name,
    .description = image_description,
    .parameters_json = image_parameters_json,
    .deinit = image_deinit,
};

hu_error_t hu_image_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                           hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_image_ctx_t *c = (hu_image_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    if (api_key && api_key_len > 0) {
        c->api_key = (char *)alloc->alloc(alloc->ctx, api_key_len + 1);
        if (!c->api_key) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->api_key, api_key, api_key_len);
        c->api_key[api_key_len] = '\0';
        c->api_key_len = api_key_len;
    }
    out->ctx = c;
    out->vtable = &image_vtable;
    return HU_OK;
}
