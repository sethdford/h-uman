#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/string.h"
#include "seaclaw/security.h"
#include "seaclaw/tools/validation.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SC_SCREENSHOT_NAME "screenshot"
#define SC_SCREENSHOT_DESC "Take a screenshot and save to workspace."
#define SC_SCREENSHOT_PARAMS "{\"type\":\"object\",\"properties\":{\"filename\":{\"type\":\"string\"}}}"
#define SC_SCREENSHOT_DEFAULT "screenshot.png"

typedef struct sc_screenshot_ctx {
    bool enabled;
    sc_security_policy_t *policy;
} sc_screenshot_ctx_t;

static sc_error_t screenshot_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_screenshot_ctx_t *sc = (sc_screenshot_ctx_t *)ctx;
    if (!out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *filename = args ? sc_json_get_string(args, "filename") : NULL;
    if (!filename || filename[0] == '\0') filename = SC_SCREENSHOT_DEFAULT;
#if SC_IS_TEST
    size_t need = 9 + strlen(filename);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    int n = snprintf(msg, need + 1, "[IMAGE:%s]", filename);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = sc_tool_result_ok_owned(msg, len);
    return SC_OK;
#else
    const char *path = filename;
#ifdef __APPLE__
    {
        const char *argv[4];
        argv[0] = "screencapture";
        argv[1] = "-x";
        argv[2] = path;
        argv[3] = NULL;
        sc_run_result_t run = {0};
        sc_error_t err = sc_process_run_with_policy(alloc, argv, NULL,
            4096, sc ? sc->policy : NULL, &run);
        sc_run_result_free(alloc, &run);
        if (err != SC_OK) {
            *out = sc_tool_result_fail("screencapture failed", 20);
            return SC_OK;
        }
        if (!run.success) {
            *out = sc_tool_result_fail("screencapture failed", 20);
            return SC_OK;
        }
    }
#else
    {
        const char *argv[5];
        argv[0] = "import";
        argv[1] = "-window";
        argv[2] = "root";
        argv[3] = path;
        argv[4] = NULL;
        sc_run_result_t run = {0};
        sc_error_t err = sc_process_run_with_policy(alloc, argv, NULL,
            4096, sc ? sc->policy : NULL, &run);
        sc_run_result_free(alloc, &run);
        if (err != SC_OK) {
            *out = sc_tool_result_fail("import failed", 12);
            return SC_OK;
        }
        if (!run.success) {
            *out = sc_tool_result_fail("import failed", 12);
            return SC_OK;
        }
    }
#endif
    size_t need = 9 + strlen(path);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    int n = snprintf(msg, need + 1, "[IMAGE:%s]", path);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = sc_tool_result_ok_owned(msg, len);
    return SC_OK;
#endif
}

static const char *screenshot_name(void *ctx) { (void)ctx; return SC_SCREENSHOT_NAME; }
static const char *screenshot_description(void *ctx) { (void)ctx; return SC_SCREENSHOT_DESC; }
static const char *screenshot_parameters_json(void *ctx) { (void)ctx; return SC_SCREENSHOT_PARAMS; }
static void screenshot_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t screenshot_vtable = {
    .execute = screenshot_execute, .name = screenshot_name,
    .description = screenshot_description, .parameters_json = screenshot_parameters_json,
    .deinit = screenshot_deinit,
};

sc_error_t sc_screenshot_create(sc_allocator_t *alloc, bool enabled,
    sc_security_policy_t *policy, sc_tool_t *out) {
    (void)alloc;
    sc_screenshot_ctx_t *c = (sc_screenshot_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->enabled = enabled;
    c->policy = policy;
    out->ctx = c;
    out->vtable = &screenshot_vtable;
    return SC_OK;
}
