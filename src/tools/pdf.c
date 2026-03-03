#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_PDF_MAX_SIZE (20 * 1024 * 1024)

#define SC_PDF_NAME "pdf"
#define SC_PDF_DESC "Extract text content and metadata from a PDF file."
#define SC_PDF_PARAMS                                                           \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","       \
    "\"description\":\"Path to PDF file\"},\"max_pages\":{\"type\":\"number\"," \
    "\"description\":\"Maximum pages to extract (default: all)\"}},\"required\":[\"path\"]}"

typedef struct sc_pdf_ctx {
    int placeholder;
} sc_pdf_ctx_t;

static sc_error_t pdf_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
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
    int max_pages = (int)sc_json_get_number(args, "max_pages", 0);

#if SC_IS_TEST
    {
        size_t need = 64 + strlen(path);
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 13);
            return SC_ERR_OUT_OF_MEMORY;
        }
        int n =
            snprintf(msg, need + 1,
                     "File: %s\nPages: 1\nContent: (test mode - PDF extraction disabled)", path);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
    }
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
    if ((unsigned long)sz > SC_PDF_MAX_SIZE) {
        fclose(f);
        *out = sc_tool_result_fail("file too large (>20MB)", 21);
        return SC_OK;
    }
    rewind(f);
    unsigned char hdr[5];
    size_t nr = fread(hdr, 1, 5, f);
    fclose(f);
    f = NULL;

    if (nr < 5 || memcmp(hdr, "%PDF-", 5) != 0) {
        *out = sc_tool_result_fail("not a PDF file", 14);
        return SC_OK;
    }

#ifdef SC_GATEWAY_POSIX
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

    sc_run_result_t res = {0};
    sc_error_t err = sc_process_run_with_policy(alloc, argv_buf, NULL, 1048576, NULL, &res);
    if (err == SC_OK && res.success && res.stdout_len > 0) {
        size_t need = 128 + strlen(path) + res.stdout_len;
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (msg) {
            int n = snprintf(msg, need + 1, "File: %s\nSize: %ld bytes\n\n%.*s", path, (long)sz,
                             (int)res.stdout_len, res.stdout_buf ? res.stdout_buf : "");
            size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
            msg[len] = '\0';
            sc_run_result_free(alloc, &res);
            *out = sc_tool_result_ok_owned(msg, len);
            return SC_OK;
        }
    }
    sc_run_result_free(alloc, &res);
#endif

    /* Fallback: metadata only */
    size_t need = 128 + strlen(path);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 13);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(
        msg, need + 1,
        "File: %s\nSize: %ld bytes\nVersion: %.5s\n(install pdftotext for text extraction)", path,
        (long)sz, (const char *)hdr);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = sc_tool_result_ok_owned(msg, len);
    return SC_OK;
#endif
}

static const char *pdf_name(void *ctx) {
    (void)ctx;
    return SC_PDF_NAME;
}
static const char *pdf_description(void *ctx) {
    (void)ctx;
    return SC_PDF_DESC;
}
static const char *pdf_parameters_json(void *ctx) {
    (void)ctx;
    return SC_PDF_PARAMS;
}
static void pdf_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    free(ctx);
}

static const sc_tool_vtable_t pdf_vtable = {
    .execute = pdf_execute,
    .name = pdf_name,
    .description = pdf_description,
    .parameters_json = pdf_parameters_json,
    .deinit = pdf_deinit,
};

sc_error_t sc_pdf_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    void *ctx = calloc(1, sizeof(sc_pdf_ctx_t));
    if (!ctx)
        return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &pdf_vtable;
    return SC_OK;
}
