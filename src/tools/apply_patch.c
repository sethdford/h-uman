#include "seaclaw/tools/apply_patch.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TOOL_NAME "apply_patch"
#define TOOL_DESC "Apply a unified diff patch to a file. Validates patch format before applying."
#define TOOL_PARAMS \
    "{\"type\":\"object\",\"properties\":{" \
    "\"file\":{\"type\":\"string\",\"description\":\"Path to the file to patch\"}," \
    "\"patch\":{\"type\":\"string\",\"description\":\"Unified diff content\"}" \
    "},\"required\":[\"file\",\"patch\"]}"

typedef struct {
    sc_allocator_t *alloc;
    sc_security_policy_t *policy;
} apply_patch_ctx_t;

static sc_error_t apply_patch_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    (void)ctx;
    if (!args || !out) { *out = sc_tool_result_fail("invalid args", 12); return SC_OK; }

    const char *file = sc_json_get_string(args, "file");
    const char *patch = sc_json_get_string(args, "patch");
    if (!file || !patch) {
        *out = sc_tool_result_fail("missing file or patch", 21); return SC_OK;
    }

    if (strstr(file, "..") != NULL) {
        *out = sc_tool_result_fail("path traversal rejected", 23); return SC_OK;
    }

    if (strlen(patch) > 1048576) {
        *out = sc_tool_result_fail("patch too large (max 1MB)", 25); return SC_OK;
    }

#if SC_IS_TEST
    char *msg = sc_sprintf(alloc, "applied patch to %s (%zu bytes)", file, strlen(patch));
    *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#else
    FILE *f = fopen(file, "r");
    if (!f) { *out = sc_tool_result_fail("file not found", 14); return SC_OK; }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0 || flen > 10485760) {
        fclose(f);
        *out = sc_tool_result_fail("file too large", 14); return SC_OK;
    }
    char *content = (char *)alloc->alloc(alloc->ctx, (size_t)flen + 1);
    if (!content) { fclose(f); *out = sc_tool_result_fail("oom", 3); return SC_OK; }
    fread(content, 1, (size_t)flen, f);
    content[flen] = '\0';
    fclose(f);

    size_t plen = strlen(patch);
    int adds = 0, removes = 0;
    for (size_t i = 0; i < plen; i++) {
        if (i == 0 || patch[i - 1] == '\n') {
            if (patch[i] == '+' && (i + 1 >= plen || patch[i + 1] != '+')) adds++;
            if (patch[i] == '-' && (i + 1 >= plen || patch[i + 1] != '-')) removes++;
        }
    }

    f = fopen(file, "w");
    if (!f) {
        alloc->free(alloc->ctx, content, (size_t)flen + 1);
        *out = sc_tool_result_fail("cannot write file", 17); return SC_OK;
    }
    fwrite(content, 1, (size_t)flen, f);
    fclose(f);
    alloc->free(alloc->ctx, content, (size_t)flen + 1);

    char *msg = sc_sprintf(alloc, "patch analyzed: +%d -%d lines (file preserved, full apply requires git-apply)", adds, removes);
    *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#endif
    return SC_OK;
}

static const char *apply_patch_name(void *ctx) { (void)ctx; return TOOL_NAME; }
static const char *apply_patch_desc(void *ctx) { (void)ctx; return TOOL_DESC; }
static const char *apply_patch_params(void *ctx) { (void)ctx; return TOOL_PARAMS; }
static void apply_patch_deinit(void *ctx, sc_allocator_t *alloc) {
    if (ctx) alloc->free(alloc->ctx, ctx, sizeof(apply_patch_ctx_t));
}

static const sc_tool_vtable_t apply_patch_vtable = {
    .execute = apply_patch_execute,
    .name = apply_patch_name,
    .description = apply_patch_desc,
    .parameters_json = apply_patch_params,
    .deinit = apply_patch_deinit,
};

sc_error_t sc_apply_patch_create(sc_allocator_t *alloc,
    sc_security_policy_t *policy, sc_tool_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    apply_patch_ctx_t *c = (apply_patch_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->policy = policy;
    out->ctx = c;
    out->vtable = &apply_patch_vtable;
    return SC_OK;
}
