/*
 * Chunk local files and POST each chunk to BFF /v1/memory/store.
 * Env: BFF_BASE_URL, BFF_AUTH_TOKEN, optional BFF_TENANT_ID.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/file.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/tool.h"
#include "human/tools/validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_DOC_INGEST_NAME "doc_ingest"
#define HU_DOC_INGEST_DESC                                                            \
    "Ingest a workspace file into cloud memory (BFF). Chunks text and POSTs to "      \
    "/v1/memory/store. PDFs use pdftotext when available. Requires BFF_BASE_URL and " \
    "BFF_AUTH_TOKEN; optional BFF_TENANT_ID."
#define HU_DOC_INGEST_PARAMS                                                                  \
    "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},\"client_id\":" \
    "{\"type\":\"string\"},\"source_type\":{\"type\":\"string\"},\"chunk_size\":{\"type\":"   \
    "\"number\",\"default\":1800},\"overlap\":{\"type\":\"number\",\"default\":200},"         \
    "\"session_id\":{\"type\":\"string\"}},\"required\":[\"file_path\",\"client_id\"]}"

#define CHUNK_DEFAULT   1800
#define OVERLAP_DEFAULT 200
#define READ_CAP        (8 * 1024 * 1024)

typedef struct {
    const char *workspace_dir;
    size_t workspace_dir_len;
    hu_security_policy_t *policy;
} doc_ingest_ctx_t;

#if !HU_IS_TEST
static void strip_base_slash(char *s) {
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/')
        s[--n] = '\0';
}

static const char *bff_bearer(void) {
    static char a[4096];
    const char *t = getenv("BFF_AUTH_TOKEN");
    if (!t || !t[0])
        return NULL;
    snprintf(a, sizeof(a), "Bearer %s", t);
    return a;
}

static int bff_store_json(hu_allocator_t *alloc, const char *base, const char *auth,
                          const char *tenant, const char *json, size_t json_len) {
    char url[768];
    snprintf(url, sizeof(url), "%s/v1/memory/store", base);
    char xt[256];
    const char *ex = NULL;
    if (tenant && tenant[0]) {
        snprintf(xt, sizeof(xt), "X-Tenant-ID: %s\n", tenant);
        ex = xt;
    }
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_post_json_ex(alloc, url, auth, ex, json, json_len, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return -1;
    }
    long sc = resp.status_code;
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    return (sc >= 200 && sc < 300) ? 0 : -1;
}
#endif

static hu_error_t doc_ingest_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                     hu_tool_result_t *out) {
    doc_ingest_ctx_t *c = (doc_ingest_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
#if HU_IS_TEST
    (void)alloc;
    *out = hu_tool_result_ok("{\"ok\":true,\"chunks_stored\":1}", 30);
    return HU_OK;
#else
    const char *fp = hu_json_get_string(args, "file_path");
    const char *client = hu_json_get_string(args, "client_id");
    if (!fp || !client) {
        *out = hu_tool_result_fail("file_path and client_id required", 32);
        return HU_OK;
    }
    const char *stype = hu_json_get_string(args, "source_type");
    if (!stype || !stype[0])
        stype = "doc";
    int chunk_sz = (int)hu_json_get_number(args, "chunk_size", CHUNK_DEFAULT);
    int overlap = (int)hu_json_get_number(args, "overlap", OVERLAP_DEFAULT);
    if (chunk_sz < 256)
        chunk_sz = 256;
    if (overlap < 0 || overlap >= chunk_sz)
        overlap = chunk_sz / 10;
    const char *sid = hu_json_get_string(args, "session_id");

    const char *eb = getenv("BFF_BASE_URL");
    const char *auth = bff_bearer();
    if (!eb || !auth) {
        *out = hu_tool_result_fail("set BFF_BASE_URL and BFF_AUTH_TOKEN", 35);
        return HU_OK;
    }
    char base[512];
    strncpy(base, eb, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    strip_base_slash(base);

    hu_error_t perr =
        hu_tool_validate_path(fp, c->workspace_dir, c->workspace_dir ? c->workspace_dir_len : 0);
    if (perr != HU_OK) {
        *out = hu_tool_result_fail("path traversal or invalid path", 30);
        return HU_OK;
    }
    char resolved[4096];
    const char *open_path = fp;
    bool is_abs =
        (fp[0] == '/') || (strlen(fp) >= 2 && fp[1] == ':' &&
                           ((fp[0] >= 'A' && fp[0] <= 'Z') || (fp[0] >= 'a' && fp[0] <= 'z')));
    if (c->workspace_dir && c->workspace_dir_len > 0 && !is_abs) {
        size_t n = c->workspace_dir_len;
        if (n >= sizeof(resolved) - 1) {
            *out = hu_tool_result_fail("path too long", 13);
            return HU_OK;
        }
        memcpy(resolved, c->workspace_dir, n);
        if (n > 0 && resolved[n - 1] != '/') {
            resolved[n] = '/';
            n++;
        }
        if (n + strlen(fp) >= sizeof(resolved)) {
            *out = hu_tool_result_fail("path too long", 13);
            return HU_OK;
        }
        memcpy(resolved + n, fp, strlen(fp) + 1);
        open_path = resolved;
    }
    if (!c->policy || !hu_security_path_allowed(c->policy, open_path, strlen(open_path))) {
        *out = hu_tool_result_fail("path not allowed by policy", 26);
        return HU_OK;
    }

    char *buf = NULL;
    size_t rd = 0;
    hu_error_t ferr = hu_file_slurp(alloc, open_path, (size_t)READ_CAP, &buf, &rd);
    if (ferr == HU_ERR_NOT_FOUND) {
        *out = hu_tool_result_fail("file not found", 14);
        return HU_OK;
    }
    if (ferr == HU_ERR_LIMIT_REACHED) {
        *out = hu_tool_result_fail("file too large or invalid", 25);
        return HU_OK;
    }
    if (ferr == HU_ERR_OUT_OF_MEMORY) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (ferr != HU_OK) {
        *out = hu_tool_result_fail("read failed", 11);
        return HU_OK;
    }
    size_t buf_cap = rd + 1;

    size_t olen = strlen(open_path);
    bool is_pdf = olen > 4 && strcmp(open_path + olen - 4, ".pdf") == 0;
    if (is_pdf && rd >= 5 && memcmp(buf, "%PDF-", 5) == 0) {
#ifdef HU_GATEWAY_POSIX
        const char *argv_buf[6];
        argv_buf[0] = "pdftotext";
        argv_buf[1] = open_path;
        argv_buf[2] = "-";
        argv_buf[3] = NULL;
        hu_run_result_t res = {0};
        hu_error_t er = hu_process_run_with_policy(alloc, argv_buf, NULL, 1048576, NULL, &res);
        if (er == HU_OK && res.success && res.stdout_len > 0) {
            alloc->free(alloc->ctx, buf, buf_cap);
            buf = (char *)alloc->alloc(alloc->ctx, res.stdout_len + 1);
            if (!buf) {
                hu_run_result_free(alloc, &res);
                *out = hu_tool_result_fail("out of memory", 12);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(buf, res.stdout_buf, res.stdout_len);
            buf[res.stdout_len] = '\0';
            rd = res.stdout_len;
            buf_cap = rd + 1;
            hu_run_result_free(alloc, &res);
        }
#endif
    }

    const char *tenant = getenv("BFF_TENANT_ID");
    int stored = 0;
    size_t off = 0;
    int part = 0;
    while (off < rd) {
        size_t take = (size_t)chunk_sz;
        if (off + take > rd)
            take = rd - off;
        size_t jb_cap = take * 2 + 512;
        char *jb = (char *)alloc->alloc(alloc->ctx, jb_cap);
        if (!jb) {
            alloc->free(alloc->ctx, buf, buf_cap);
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        char keyb[384];
        snprintf(keyb, sizeof(keyb), "%s:%s:%d", client, stype, part);
        size_t j = 0;
        j = hu_buf_appendf(jb, jb_cap, j, "{\"key\":\"%s\",\"content\":\"", keyb);
        for (size_t k = 0; k < take; k++) {
            unsigned char ch = (unsigned char)buf[off + k];
            if (ch == '"' || ch == '\\') {
                if (j + 2 < jb_cap)
                    j = hu_buf_appendf(jb, jb_cap, j, "\\%c", (char)ch);
            } else if (ch < 32) {
                if (j + 1 < jb_cap)
                    jb[j++] = ' ';
            } else {
                if (j + 1 < jb_cap)
                    jb[j++] = (char)ch;
            }
        }
        j = hu_buf_appendf(jb, jb_cap, j, "\"");
        if (sid && sid[0])
            j = hu_buf_appendf(jb, jb_cap, j, ",\"session_id\":\"%s\"", sid);
        j = hu_buf_appendf(jb, jb_cap, j, "}");

        if (bff_store_json(alloc, base, auth, tenant, jb, j) != 0) {
            alloc->free(alloc->ctx, jb, jb_cap);
            alloc->free(alloc->ctx, buf, buf_cap);
            *out = hu_tool_result_fail("bff store failed", 16);
            return HU_OK;
        }
        alloc->free(alloc->ctx, jb, jb_cap);
        stored++;
        part++;
        if (off + take >= rd)
            break;
        off += (size_t)chunk_sz - (size_t)overlap;
    }
    alloc->free(alloc->ctx, buf, buf_cap);
    char *summary = hu_sprintf(alloc, "{\"ok\":true,\"chunks_stored\":%d}", stored);
    *out = hu_tool_result_ok_owned(summary, summary ? strlen(summary) : 0);
    return HU_OK;
#endif
}

static const char *doc_ingest_name(void *ctx) {
    (void)ctx;
    return HU_DOC_INGEST_NAME;
}
static const char *doc_ingest_description(void *ctx) {
    (void)ctx;
    return HU_DOC_INGEST_DESC;
}
static const char *doc_ingest_parameters_json(void *ctx) {
    (void)ctx;
    return HU_DOC_INGEST_PARAMS;
}
static void doc_ingest_deinit(void *ctx, hu_allocator_t *alloc) {
    doc_ingest_ctx_t *c = (doc_ingest_ctx_t *)ctx;
    if (!c || !alloc)
        return;
    if (c->workspace_dir)
        alloc->free(alloc->ctx, (void *)c->workspace_dir, c->workspace_dir_len + 1);
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_tool_vtable_t doc_ingest_vtable = {
    .execute = doc_ingest_execute,
    .name = doc_ingest_name,
    .description = doc_ingest_description,
    .parameters_json = doc_ingest_parameters_json,
    .deinit = doc_ingest_deinit,
};

hu_error_t hu_doc_ingest_create(hu_allocator_t *alloc, const char *workspace_dir,
                                size_t workspace_dir_len, hu_security_policy_t *policy,
                                hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    doc_ingest_ctx_t *c = (doc_ingest_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    if (workspace_dir && workspace_dir_len > 0) {
        c->workspace_dir = hu_strndup(alloc, workspace_dir, workspace_dir_len);
        if (!c->workspace_dir) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->workspace_dir_len = workspace_dir_len;
    }
    c->policy = policy;
    out->ctx = c;
    out->vtable = &doc_ingest_vtable;
    return HU_OK;
}
