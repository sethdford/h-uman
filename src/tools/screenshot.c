#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/security.h"
#include "human/tool.h"
#include "human/tools/validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_SCREENSHOT_NAME "screenshot"
#define HU_SCREENSHOT_DESC "Take a screenshot and save to workspace."
#define HU_SCREENSHOT_PARAMS \
    "{\"type\":\"object\",\"properties\":{\"filename\":{\"type\":\"string\"}}}"
#define HU_SCREENSHOT_DEFAULT "screenshot.png"

typedef struct hu_screenshot_ctx {
    bool enabled;
    hu_security_policy_t *policy;
} hu_screenshot_ctx_t;

static hu_error_t screenshot_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                     hu_tool_result_t *out) {
    hu_screenshot_ctx_t *sc = (hu_screenshot_ctx_t *)ctx;
    (void)sc;
    if (!out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *filename = args ? hu_json_get_string(args, "filename") : NULL;
    if (!filename || filename[0] == '\0')
        filename = HU_SCREENSHOT_DEFAULT;
    if (hu_tool_validate_path(filename, NULL, 0) != HU_OK) {
        *out = hu_tool_result_fail("filename not allowed", 20);
        return HU_OK;
    }
#if HU_IS_TEST
    size_t need = 9 + strlen(filename);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, need + 1, "[IMAGE:%s]", filename);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = hu_tool_result_ok_owned(msg, len);
    return HU_OK;
#else
    const char *path = filename;
#ifdef __APPLE__
    {
        const char *argv[4];
        argv[0] = "screencapture";
        argv[1] = "-x";
        argv[2] = path;
        argv[3] = NULL;
        hu_run_result_t run = {0};
        hu_error_t err =
            hu_process_run_with_policy(alloc, argv, NULL, 4096, sc ? sc->policy : NULL, &run);
        hu_run_result_free(alloc, &run);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("screencapture failed", 20);
            return HU_OK;
        }
        if (!run.success) {
            *out = hu_tool_result_fail("screencapture failed", 20);
            return HU_OK;
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
        hu_run_result_t run = {0};
        hu_error_t err =
            hu_process_run_with_policy(alloc, argv, NULL, 4096, sc ? sc->policy : NULL, &run);
        hu_run_result_free(alloc, &run);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("import failed", 12);
            return HU_OK;
        }
        if (!run.success) {
            *out = hu_tool_result_fail("import failed", 12);
            return HU_OK;
        }
    }
#endif
    size_t need = 9 + strlen(path);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, need + 1, "[IMAGE:%s]", path);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = hu_tool_result_ok_owned(msg, len);
    return HU_OK;
#endif
}

static const char *screenshot_name(void *ctx) {
    (void)ctx;
    return HU_SCREENSHOT_NAME;
}
static const char *screenshot_description(void *ctx) {
    (void)ctx;
    return HU_SCREENSHOT_DESC;
}
static const char *screenshot_parameters_json(void *ctx) {
    (void)ctx;
    return HU_SCREENSHOT_PARAMS;
}
static void screenshot_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(hu_screenshot_ctx_t));
}

static const hu_tool_vtable_t screenshot_vtable = {
    .execute = screenshot_execute,
    .name = screenshot_name,
    .description = screenshot_description,
    .parameters_json = screenshot_parameters_json,
    .deinit = screenshot_deinit,
};

hu_error_t hu_screenshot_create(hu_allocator_t *alloc, bool enabled, hu_security_policy_t *policy,
                                hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_screenshot_ctx_t *c = (hu_screenshot_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->enabled = enabled;
    c->policy = policy;
    out->ctx = c;
    out->vtable = &screenshot_vtable;
    return HU_OK;
}
