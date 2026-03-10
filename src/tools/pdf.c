#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/tool.h"
#include "human/tools/validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_PDF_MAX_SIZE (20 * 1024 * 1024)

#define HU_PDF_NAME "pdf"
#define HU_PDF_DESC "Extract text content and metadata from a PDF file."
#define HU_PDF_PARAMS                                                           \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","       \
    "\"description\":\"Path to PDF file\"},\"max_pages\":{\"type\":\"number\"," \
    "\"description\":\"Maximum pages to extract (default: all)\"}},\"required\":[\"path\"]}"

typedef struct hu_pdf_ctx {
    char _unused;
} hu_pdf_ctx_t;

static hu_error_t pdf_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
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
    {
        size_t need = 64 + strlen(path);
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        int n =
            snprintf(msg, need + 1,
                     "File: %s\nPages: 1\nContent: (test mode - PDF extraction disabled)", path);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = hu_tool_result_ok_owned(msg, len);
    }
    return HU_OK;
#else
    int max_pages = (int)hu_json_get_number(args, "max_pages", 0);
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
    if ((unsigned long)sz > HU_PDF_MAX_SIZE) {
        fclose(f);
        *out = hu_tool_result_fail("file too large (>20MB)", 21);
        return HU_OK;
    }
    rewind(f);
    unsigned char hdr[5];
    size_t nr = fread(hdr, 1, 5, f);
    fclose(f);
    f = NULL;

    if (nr < 5 || memcmp(hdr, "%PDF-", 5) != 0) {
        *out = hu_tool_result_fail("not a PDF file", 14);
        return HU_OK;
    }

#ifdef HU_GATEWAY_POSIX
    const char *argv_buf[8];
    size_t ai = 0;
    argv_buf[ai++] = "pdftotext";
    char page_buf[16];
    if (max_pages > 0) {
        snprintf(page_buf, sizeof(page_buf), "%d", (int)max_pages);
        argv_buf[ai++] = "-l";
        argv_buf[ai++] = page_buf;
    }
    argv_buf[ai++] = path;
    argv_buf[ai++] = "-";
    argv_buf[ai] = NULL;

    hu_run_result_t res = {0};
    hu_error_t err = hu_process_run_with_policy(alloc, argv_buf, NULL, 1048576, NULL, &res);
    if (err == HU_OK && res.success && res.stdout_len > 0) {
        size_t need = 128 + strlen(path) + res.stdout_len;
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (msg) {
            int n = snprintf(msg, need + 1, "File: %s\nSize: %ld bytes\n\n%.*s", path, (long)sz,
                             (int)res.stdout_len, res.stdout_buf ? res.stdout_buf : "");
            size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
            msg[len] = '\0';
            hu_run_result_free(alloc, &res);
            *out = hu_tool_result_ok_owned(msg, len);
            return HU_OK;
        }
    }
    hu_run_result_free(alloc, &res);
#endif

    /* Fallback: metadata only */
    size_t need = 128 + strlen(path);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(
        msg, need + 1,
        "File: %s\nSize: %ld bytes\nVersion: %.5s\n(install pdftotext for text extraction)", path,
        (long)sz, (const char *)hdr);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = hu_tool_result_ok_owned(msg, len);
    return HU_OK;
#endif
}

static const char *pdf_name(void *ctx) {
    (void)ctx;
    return HU_PDF_NAME;
}
static const char *pdf_description(void *ctx) {
    (void)ctx;
    return HU_PDF_DESC;
}
static const char *pdf_parameters_json(void *ctx) {
    (void)ctx;
    return HU_PDF_PARAMS;
}
static void pdf_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(hu_pdf_ctx_t));
}

static const hu_tool_vtable_t pdf_vtable = {
    .execute = pdf_execute,
    .name = pdf_name,
    .description = pdf_description,
    .parameters_json = pdf_parameters_json,
    .deinit = pdf_deinit,
};

hu_error_t hu_pdf_create(hu_allocator_t *alloc, hu_tool_t *out) {
    hu_pdf_ctx_t *ctx = (hu_pdf_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_pdf_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(hu_pdf_ctx_t));
    out->ctx = ctx;
    out->vtable = &pdf_vtable;
    return HU_OK;
}
