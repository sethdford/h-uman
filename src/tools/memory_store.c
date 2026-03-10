#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/tool.h"
#include <stdlib.h>
#include <string.h>

#define HU_MEMORY_STORE_NAME "memory_store"
#define HU_MEMORY_STORE_DESC "Store to memory"
#define HU_MEMORY_STORE_PARAMS                                                                  \
    "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"content\":{\"type\":" \
    "\"string\"}},\"required\":[\"key\",\"content\"]}"
#define HU_MEMORY_KEY_MAX     1024
#define HU_MEMORY_CONTENT_MAX 256000

typedef struct hu_memory_store_ctx {
    hu_memory_t *memory;
} hu_memory_store_ctx_t;

static hu_error_t memory_store_execute(void *ctx, hu_allocator_t *alloc,
                                       const hu_json_value_t *args, hu_tool_result_t *out) {
    hu_memory_store_ctx_t *c = (hu_memory_store_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *key = hu_json_get_string(args, "key");
    const char *content = hu_json_get_string(args, "content");
    if (!key || strlen(key) == 0) {
        *out = hu_tool_result_fail("missing key", 11);
        return HU_OK;
    }
    if (strlen(key) > HU_MEMORY_KEY_MAX) {
        *out = hu_tool_result_fail("key too long", 12);
        return HU_OK;
    }
    if (!content)
        content = "";
    if (strlen(content) > HU_MEMORY_CONTENT_MAX) {
        *out = hu_tool_result_fail("content too long", 16);
        return HU_OK;
    }
#if HU_IS_TEST
    char *msg = hu_strndup(alloc, "(memory_store stub)", 19);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, 19);
    return HU_OK;
#else
    if (!c->memory || !c->memory->vtable) {
        *out = hu_tool_result_fail("memory not configured", 21);
        return HU_OK;
    }
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CUSTOM};
    const char *sid = c->memory->current_session_id;
    size_t sid_len = c->memory->current_session_id_len;
    hu_error_t err = c->memory->vtable->store(c->memory->ctx, key, strlen(key), content,
                                              strlen(content), &cat, sid, sid_len);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("store failed", 12);
        return HU_OK;
    }
    char *ok = hu_strndup(alloc, "stored", 6);
    if (!ok) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(ok, 6);
    return HU_OK;
#endif
}

static const char *memory_store_name(void *ctx) {
    (void)ctx;
    return HU_MEMORY_STORE_NAME;
}
static const char *memory_store_description(void *ctx) {
    (void)ctx;
    return HU_MEMORY_STORE_DESC;
}
static const char *memory_store_parameters_json(void *ctx) {
    (void)ctx;
    return HU_MEMORY_STORE_PARAMS;
}
static void memory_store_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(hu_memory_store_ctx_t));
}

static const hu_tool_vtable_t memory_store_vtable = {
    .execute = memory_store_execute,
    .name = memory_store_name,
    .description = memory_store_description,
    .parameters_json = memory_store_parameters_json,
    .deinit = memory_store_deinit,
};

hu_error_t hu_memory_store_create(hu_allocator_t *alloc, hu_memory_t *memory, hu_tool_t *out) {
    hu_memory_store_ctx_t *c = (hu_memory_store_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->memory = memory;
    out->ctx = c;
    out->vtable = &memory_store_vtable;
    return HU_OK;
}
