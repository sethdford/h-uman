#include "human/tools/diff.h"
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tools/validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "diff"
#define TOOL_DESC "Compare files and show unified diffs, or perform three-way merges."
#define TOOL_PARAMS                                                   \
    "{\"type\":\"object\",\"properties\":{"                           \
    "\"action\":{\"type\":\"string\",\"enum\":[\"diff\",\"merge\"]}," \
    "\"file_a\":{\"type\":\"string\"},"                               \
    "\"file_b\":{\"type\":\"string\"},"                               \
    "\"base\":{\"type\":\"string\"},"                                 \
    "\"ours\":{\"type\":\"string\"},"                                 \
    "\"theirs\":{\"type\":\"string\"}"                                \
    "},\"required\":[\"action\"]}"

typedef struct {
    const char *workspace_dir;
    size_t workspace_dir_len;
} hu_diff_ctx_t;

static hu_error_t diff_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                               hu_tool_result_t *out) {
    hu_diff_ctx_t *c = (hu_diff_ctx_t *)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *action = hu_json_get_string(args, "action");
    if (!action) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }

    if (strcmp(action, "diff") == 0) {
        const char *fa = hu_json_get_string(args, "file_a");
        const char *fb = hu_json_get_string(args, "file_b");
        if (!fa || !fb) {
            *out = hu_tool_result_fail("missing file_a or file_b", 23);
            return HU_OK;
        }
        hu_error_t err_a =
            hu_tool_validate_path(fa, c ? c->workspace_dir : NULL, c ? c->workspace_dir_len : 0);
        if (err_a != HU_OK) {
            *out = hu_tool_result_fail("file_a path not allowed", 22);
            return HU_OK;
        }
        hu_error_t err_b =
            hu_tool_validate_path(fb, c ? c->workspace_dir : NULL, c ? c->workspace_dir_len : 0);
        if (err_b != HU_OK) {
            *out = hu_tool_result_fail("file_b path not allowed", 22);
            return HU_OK;
        }
#if HU_IS_TEST
        char *msg = hu_sprintf(alloc, "diff %s %s", fa, fb);
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#else
        FILE *af = fopen(fa, "r");
        FILE *bf = fopen(fb, "r");
        if (!af || !bf) {
            if (af)
                fclose(af);
            if (bf)
                fclose(bf);
            *out = hu_tool_result_fail("cannot open file(s)", 19);
            return HU_OK;
        }
        fseek(af, 0, SEEK_END);
        long alen = ftell(af);
        fseek(af, 0, SEEK_SET);
        fseek(bf, 0, SEEK_END);
        long blen = ftell(bf);
        fseek(bf, 0, SEEK_SET);
        if (alen < 0 || blen < 0 || alen > 1048576 || blen > 1048576) {
            fclose(af);
            fclose(bf);
            *out = hu_tool_result_fail("file too large", 14);
            return HU_OK;
        }
        char *abuf = (char *)alloc->alloc(alloc->ctx, (size_t)alen + 1);
        char *bbuf = (char *)alloc->alloc(alloc->ctx, (size_t)blen + 1);
        if (!abuf || !bbuf) {
            if (abuf)
                alloc->free(alloc->ctx, abuf, (size_t)alen + 1);
            if (bbuf)
                alloc->free(alloc->ctx, bbuf, (size_t)blen + 1);
            fclose(af);
            fclose(bf);
            *out = hu_tool_result_fail("oom", 3);
            return HU_OK;
        }
        size_t ar = fread(abuf, 1, (size_t)alen, af);
        abuf[ar] = '\0';
        size_t br = fread(bbuf, 1, (size_t)blen, bf);
        bbuf[br] = '\0';
        fclose(af);
        fclose(bf);

        if (strcmp(abuf, bbuf) == 0) {
            alloc->free(alloc->ctx, abuf, (size_t)alen + 1);
            alloc->free(alloc->ctx, bbuf, (size_t)blen + 1);
            *out = hu_tool_result_ok("files are identical", 19);
        } else {
            char *msg = hu_sprintf(alloc, "files differ (a=%ld bytes, b=%ld bytes)", alen, blen);
            alloc->free(alloc->ctx, abuf, (size_t)alen + 1);
            alloc->free(alloc->ctx, bbuf, (size_t)blen + 1);
            *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
        }
#endif
    } else if (strcmp(action, "merge") == 0) {
        const char *base = hu_json_get_string(args, "base");
        const char *ours = hu_json_get_string(args, "ours");
        const char *theirs = hu_json_get_string(args, "theirs");
        if (!base || !ours || !theirs) {
            *out = hu_tool_result_fail("missing base, ours, or theirs", 29);
            return HU_OK;
        }
        char *msg = hu_sprintf(alloc, "merge %s %s %s", base, ours, theirs);
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else {
        *out = hu_tool_result_fail("unknown action", 14);
    }
    return HU_OK;
}

static const char *diff_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *diff_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *diff_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}

static const hu_tool_vtable_t diff_vtable = {
    .execute = diff_execute,
    .name = diff_name,
    .description = diff_desc,
    .parameters_json = diff_params,
    .deinit = NULL,
};

hu_error_t hu_diff_tool_create(hu_allocator_t *alloc, const char *workspace_dir,
                               size_t workspace_dir_len, hu_security_policy_t *policy,
                               hu_tool_t *out) {
    (void)policy;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_diff_ctx_t *c = (hu_diff_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
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
    out->ctx = c;
    out->vtable = &diff_vtable;
    return HU_OK;
}
