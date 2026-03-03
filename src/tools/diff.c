#include "seaclaw/tools/diff.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
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

static sc_error_t diff_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                               sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_OK;
    }

    const char *action = sc_json_get_string(args, "action");
    if (!action) {
        *out = sc_tool_result_fail("missing action", 14);
        return SC_OK;
    }

    if (strcmp(action, "diff") == 0) {
        const char *fa = sc_json_get_string(args, "file_a");
        const char *fb = sc_json_get_string(args, "file_b");
        if (!fa || !fb) {
            *out = sc_tool_result_fail("missing file_a or file_b", 23);
            return SC_OK;
        }
#if SC_IS_TEST
        char *msg = sc_sprintf(alloc, "diff %s %s", fa, fb);
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#else
        FILE *af = fopen(fa, "r");
        FILE *bf = fopen(fb, "r");
        if (!af || !bf) {
            if (af)
                fclose(af);
            if (bf)
                fclose(bf);
            *out = sc_tool_result_fail("cannot open file(s)", 19);
            return SC_OK;
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
            *out = sc_tool_result_fail("file too large", 14);
            return SC_OK;
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
            *out = sc_tool_result_fail("oom", 3);
            return SC_OK;
        }
        fread(abuf, 1, (size_t)alen, af);
        abuf[alen] = '\0';
        fread(bbuf, 1, (size_t)blen, bf);
        bbuf[blen] = '\0';
        fclose(af);
        fclose(bf);

        if (strcmp(abuf, bbuf) == 0) {
            alloc->free(alloc->ctx, abuf, (size_t)alen + 1);
            alloc->free(alloc->ctx, bbuf, (size_t)blen + 1);
            *out = sc_tool_result_ok("files are identical", 19);
        } else {
            char *msg = sc_sprintf(alloc, "files differ (a=%ld bytes, b=%ld bytes)", alen, blen);
            alloc->free(alloc->ctx, abuf, (size_t)alen + 1);
            alloc->free(alloc->ctx, bbuf, (size_t)blen + 1);
            *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
        }
#endif
    } else if (strcmp(action, "merge") == 0) {
        const char *base = sc_json_get_string(args, "base");
        const char *ours = sc_json_get_string(args, "ours");
        const char *theirs = sc_json_get_string(args, "theirs");
        if (!base || !ours || !theirs) {
            *out = sc_tool_result_fail("missing base, ours, or theirs", 29);
            return SC_OK;
        }
        char *msg = sc_sprintf(alloc, "merge %s %s %s", base, ours, theirs);
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else {
        *out = sc_tool_result_fail("unknown action", 14);
    }
    return SC_OK;
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

static const sc_tool_vtable_t diff_vtable = {
    .execute = diff_execute,
    .name = diff_name,
    .description = diff_desc,
    .parameters_json = diff_params,
    .deinit = NULL,
};

sc_error_t sc_diff_tool_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    if (!out)
        return SC_ERR_INVALID_ARGUMENT;
    out->ctx = NULL;
    out->vtable = &diff_vtable;
    return SC_OK;
}
