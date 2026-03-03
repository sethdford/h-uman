#include "seaclaw/tools/apply_patch.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "apply_patch"
#define TOOL_DESC "Apply a unified diff patch to a file. Validates patch format before applying."
#define TOOL_PARAMS                                                                 \
    "{\"type\":\"object\",\"properties\":{"                                         \
    "\"file\":{\"type\":\"string\",\"description\":\"Path to the file to patch\"}," \
    "\"patch\":{\"type\":\"string\",\"description\":\"Unified diff content\"}"      \
    "},\"required\":[\"file\",\"patch\"]}"

typedef struct {
    sc_allocator_t *alloc;
    sc_security_policy_t *policy;
} apply_patch_ctx_t;

static sc_error_t apply_patch_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                      sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_OK;
    }
    const char *file = sc_json_get_string(args, "file");
    const char *patch = sc_json_get_string(args, "patch");
    if (!file || !patch) {
        *out = sc_tool_result_fail("missing file or patch", 21);
        return SC_OK;
    }
    if (strstr(file, "..") != NULL) {
        *out = sc_tool_result_fail("path traversal rejected", 23);
        return SC_OK;
    }
    if (strlen(patch) > 1048576) {
        *out = sc_tool_result_fail("patch too large (max 1MB)", 25);
        return SC_OK;
    }

#if SC_IS_TEST
    char *msg = sc_sprintf(alloc, "applied patch to %s (%zu bytes)", file, strlen(patch));
    *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#else
    FILE *f = fopen(file, "r");
    if (!f) {
        *out = sc_tool_result_fail("file not found", 14);
        return SC_OK;
    }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0 || flen > 10485760) {
        fclose(f);
        *out = sc_tool_result_fail("file too large", 14);
        return SC_OK;
    }
    size_t cap = (size_t)flen + strlen(patch) + 4096;
    char *lines_buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!lines_buf) {
        fclose(f);
        *out = sc_tool_result_fail("oom", 3);
        return SC_OK;
    }
    size_t line_count = 0, max_lines = 65536;
    char **lines = (char **)alloc->alloc(alloc->ctx, max_lines * sizeof(char *));
    size_t *lens = (size_t *)alloc->alloc(alloc->ctx, max_lines * sizeof(size_t));
    if (!lines || !lens) {
        if (lines)
            alloc->free(alloc->ctx, lines, max_lines * sizeof(char *));
        if (lens)
            alloc->free(alloc->ctx, lens, max_lines * sizeof(size_t));
        alloc->free(alloc->ctx, lines_buf, cap);
        fclose(f);
        *out = sc_tool_result_fail("oom", 3);
        return SC_OK;
    }
    size_t buf_off = 0, line_start = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF && buf_off < cap - 1) {
        lines_buf[buf_off++] = (char)ch;
        if (ch == '\n' || buf_off == cap - 1) {
            if (line_count < max_lines) {
                lines[line_count] = &lines_buf[line_start];
                lens[line_count] = buf_off - line_start;
                line_count++;
            }
            line_start = buf_off;
        }
    }
    if (buf_off > line_start && line_count < max_lines) {
        lines[line_count] = &lines_buf[line_start];
        lens[line_count] = buf_off - line_start;
        line_count++;
    }
    fclose(f);
    int hunks_applied = 0, hunks_failed = 0;
    const char *p = patch;
    while (*p) {
        while (*p && !(*p == '@' && *(p + 1) == '@'))
            p++;
        if (!*p)
            break;
        int old_start = 0, old_count = 1;
        if (sscanf(p, "@@ -%d,%d", &old_start, &old_count) < 1) {
            if (sscanf(p, "@@ -%d", &old_start) < 1) {
                p++;
                continue;
            }
        }
        (void)old_count;
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
        if (old_start < 1 || (size_t)old_start > line_count + 1) {
            hunks_failed++;
            while (*p && !(*p == '@' && *(p + 1) == '@'))
                p++;
            continue;
        }
        size_t insert_at = (size_t)(old_start - 1);
        char **nl = (char **)alloc->alloc(alloc->ctx, (max_lines + 4096) * sizeof(char *));
        size_t *nle = (size_t *)alloc->alloc(alloc->ctx, (max_lines + 4096) * sizeof(size_t));
        if (!nl || !nle) {
            if (nl)
                alloc->free(alloc->ctx, nl, (max_lines + 4096) * sizeof(char *));
            if (nle)
                alloc->free(alloc->ctx, nle, (max_lines + 4096) * sizeof(size_t));
            hunks_failed++;
            while (*p && !(*p == '@' && *(p + 1) == '@'))
                p++;
            continue;
        }
        for (size_t i = 0; i < insert_at && i < line_count; i++) {
            nl[i] = lines[i];
            nle[i] = lens[i];
        }
        size_t ni = insert_at, oi = insert_at;
        while (*p && !(*p == '@' && *(p + 1) == '@')) {
            const char *le = p;
            while (*le && *le != '\n')
                le++;
            size_t ll = (size_t)(le - p);
            if (*p == '-') {
                if (oi < line_count)
                    oi++;
                p = *le ? le + 1 : le;
            } else if (*p == '+') {
                nl[ni] = (char *)p + 1;
                nle[ni] = ll > 0 ? ll - 1 : 0;
                ni++;
                p = *le ? le + 1 : le;
            } else if (*p == ' ' || *p == '\\') {
                if (oi < line_count) {
                    nl[ni] = lines[oi];
                    nle[ni] = lens[oi];
                    ni++;
                    oi++;
                }
                p = *le ? le + 1 : le;
            } else
                break;
        }
        for (size_t i = oi; i < line_count; i++) {
            nl[ni] = lines[i];
            nle[ni] = lens[i];
            ni++;
        }
        alloc->free(alloc->ctx, lines, max_lines * sizeof(char *));
        alloc->free(alloc->ctx, lens, max_lines * sizeof(size_t));
        lines = nl;
        lens = nle;
        line_count = ni;
        max_lines += 4096;
        hunks_applied++;
    }
    f = fopen(file, "w");
    if (!f) {
        alloc->free(alloc->ctx, lines, max_lines * sizeof(char *));
        alloc->free(alloc->ctx, lens, max_lines * sizeof(size_t));
        alloc->free(alloc->ctx, lines_buf, cap);
        *out = sc_tool_result_fail("cannot write file", 17);
        return SC_OK;
    }
    for (size_t i = 0; i < line_count; i++) {
        fwrite(lines[i], 1, lens[i], f);
        if (lens[i] == 0 || lines[i][lens[i] - 1] != '\n')
            fputc('\n', f);
    }
    fclose(f);
    alloc->free(alloc->ctx, lines, max_lines * sizeof(char *));
    alloc->free(alloc->ctx, lens, max_lines * sizeof(size_t));
    alloc->free(alloc->ctx, lines_buf, cap);
    char *msg = sc_sprintf(alloc, "patch applied: %d hunk(s) succeeded, %d failed", hunks_applied,
                           hunks_failed);
    *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#endif
    return SC_OK;
}

static const char *apply_patch_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *apply_patch_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *apply_patch_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void apply_patch_deinit(void *ctx, sc_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(apply_patch_ctx_t));
}
static const sc_tool_vtable_t apply_patch_vtable = {.execute = apply_patch_execute,
                                                    .name = apply_patch_name,
                                                    .description = apply_patch_desc,
                                                    .parameters_json = apply_patch_params,
                                                    .deinit = apply_patch_deinit};

sc_error_t sc_apply_patch_create(sc_allocator_t *alloc, sc_security_policy_t *policy,
                                 sc_tool_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    apply_patch_ctx_t *c = (apply_patch_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->policy = policy;
    out->ctx = c;
    out->vtable = &apply_patch_vtable;
    return SC_OK;
}
