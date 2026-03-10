#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/tool.h"
#include <stdlib.h>
#include <string.h>

#include "human/tools/schema_common.h"
#define HU_MEMORY_FORGET_NAME   "memory_forget"
#define HU_MEMORY_FORGET_DESC   "Forget memory by key"
#define HU_MEMORY_FORGET_PARAMS HU_SCHEMA_KEY_ONLY
#define HU_MEMORY_KEY_MAX       1024

typedef struct hu_memory_forget_ctx {
    hu_memory_t *memory;
} hu_memory_forget_ctx_t;

static hu_error_t memory_forget_execute(void *ctx, hu_allocator_t *alloc,
                                        const hu_json_value_t *args, hu_tool_result_t *out) {
    hu_memory_forget_ctx_t *c = (hu_memory_forget_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *key = hu_json_get_string(args, "key");
    if (!key || strlen(key) == 0) {
        *out = hu_tool_result_fail("missing key", 11);
        return HU_OK;
    }
    if (strlen(key) > HU_MEMORY_KEY_MAX) {
        *out = hu_tool_result_fail("key too long", 12);
        return HU_OK;
    }
#if HU_IS_TEST
    char *msg = hu_strndup(alloc, "(memory_forget stub)", 20);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, 20);
    return HU_OK;
#else
    if (!c->memory || !c->memory->vtable) {
        *out = hu_tool_result_fail("memory not configured", 21);
        return HU_OK;
    }
    bool deleted = false;
    hu_error_t err = c->memory->vtable->forget(c->memory->ctx, key, strlen(key), &deleted);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("forget failed", 13);
        return HU_OK;
    }
    const char *msg = deleted ? "forgotten" : "not found";
    size_t msg_len = strlen(msg);
    char *ok = hu_strndup(alloc, msg, msg_len);
    if (!ok) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(ok, msg_len);
    return HU_OK;
#endif
}

static const char *memory_forget_name(void *ctx) {
    (void)ctx;
    return HU_MEMORY_FORGET_NAME;
}
static const char *memory_forget_description(void *ctx) {
    (void)ctx;
    return HU_MEMORY_FORGET_DESC;
}
static const char *memory_forget_parameters_json(void *ctx) {
    (void)ctx;
    return HU_MEMORY_FORGET_PARAMS;
}
static void memory_forget_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(hu_memory_forget_ctx_t));
}

static const hu_tool_vtable_t memory_forget_vtable = {
    .execute = memory_forget_execute,
    .name = memory_forget_name,
    .description = memory_forget_description,
    .parameters_json = memory_forget_parameters_json,
    .deinit = memory_forget_deinit,
};

hu_error_t hu_memory_forget_create(hu_allocator_t *alloc, hu_memory_t *memory, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_memory_forget_ctx_t *c = (hu_memory_forget_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->memory = memory;
    out->ctx = c;
    out->vtable = &memory_forget_vtable;
    return HU_OK;
}
