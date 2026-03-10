#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/tool.h"
#include <stdlib.h>
#include <string.h>

#include "human/tools/schema_common.h"
#define HU_MEMORY_LIST_NAME   "memory_list"
#define HU_MEMORY_LIST_DESC   "List memories"
#define HU_MEMORY_LIST_PARAMS HU_SCHEMA_EMPTY

typedef struct hu_memory_list_ctx {
    hu_memory_t *memory;
} hu_memory_list_ctx_t;

static hu_error_t memory_list_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                      hu_tool_result_t *out) {
    hu_memory_list_ctx_t *c = (hu_memory_list_ctx_t *)ctx;
    (void)args;
    if (!c || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
#if HU_IS_TEST
    char *msg = hu_strndup(alloc, "(memory_list stub)", 18);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_OK;
    }
    *out = hu_tool_result_ok_owned(msg, 18);
    return HU_OK;
#else
    if (!c->memory || !c->memory->vtable) {
        *out = hu_tool_result_fail("memory not configured", 21);
        return HU_OK;
    }
    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err =
        c->memory->vtable->list(c->memory->ctx, alloc, NULL, NULL, 0, &entries, &count);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("list failed", 11);
        return HU_OK;
    }
    char *ok = hu_strndup(alloc, "list complete", 13);
    if (!ok) {
        if (entries) {
            for (size_t i = 0; i < count; i++)
                hu_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        }
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_OK;
    }
    if (entries) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
    }
    *out = hu_tool_result_ok_owned(ok, 13);
    return HU_OK;
#endif
}

static const char *memory_list_name(void *ctx) {
    (void)ctx;
    return HU_MEMORY_LIST_NAME;
}
static const char *memory_list_description(void *ctx) {
    (void)ctx;
    return HU_MEMORY_LIST_DESC;
}
static const char *memory_list_parameters_json(void *ctx) {
    (void)ctx;
    return HU_MEMORY_LIST_PARAMS;
}
static void memory_list_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(hu_memory_list_ctx_t));
}

static const hu_tool_vtable_t memory_list_vtable = {
    .execute = memory_list_execute,
    .name = memory_list_name,
    .description = memory_list_description,
    .parameters_json = memory_list_parameters_json,
    .deinit = memory_list_deinit,
};

hu_error_t hu_memory_list_create(hu_allocator_t *alloc, hu_memory_t *memory, hu_tool_t *out) {
    hu_memory_list_ctx_t *c = (hu_memory_list_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->memory = memory;
    out->ctx = c;
    out->vtable = &memory_list_vtable;
    return HU_OK;
}
