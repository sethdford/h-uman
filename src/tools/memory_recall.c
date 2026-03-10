#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/tool.h"
#include <stdlib.h>
#include <string.h>

#define HU_MEMORY_RECALL_NAME "memory_recall"
#define HU_MEMORY_RECALL_DESC "Recall from memory"
#define HU_MEMORY_RECALL_PARAMS                                                                 \
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"limit\":{\"type\":" \
    "\"number\"}},\"required\":[\"query\"]}"
#define HU_MEMORY_QUERY_MAX 4096
#define HU_MEMORY_LIMIT_MAX 100

typedef struct hu_memory_recall_ctx {
    hu_memory_t *memory;
} hu_memory_recall_ctx_t;

static hu_error_t memory_recall_execute(void *ctx, hu_allocator_t *alloc,
                                        const hu_json_value_t *args, hu_tool_result_t *out) {
    hu_memory_recall_ctx_t *c = (hu_memory_recall_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *query = hu_json_get_string(args, "query");
    if (!query || strlen(query) == 0) {
        *out = hu_tool_result_fail("missing query", 13);
        return HU_OK;
    }
    if (strlen(query) > HU_MEMORY_QUERY_MAX) {
        *out = hu_tool_result_fail("query too long", 14);
        return HU_OK;
    }
#if HU_IS_TEST
    char *msg = hu_strndup(alloc, "(memory_recall stub)", 20);
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
    double limit_val = hu_json_get_number(args, "limit", 10);
    if (limit_val < 0 || limit_val > HU_MEMORY_LIMIT_MAX)
        limit_val = 10;
    size_t limit = (size_t)limit_val;
    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    const char *sid = c->memory->current_session_id;
    size_t sid_len = c->memory->current_session_id_len;
    hu_error_t err = c->memory->vtable->recall(c->memory->ctx, alloc, query, strlen(query), limit,
                                               sid, sid_len, &entries, &count);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("recall failed", 13);
        return HU_OK;
    }
    char *ok = hu_strndup(alloc, "recall complete", 15);
    if (!ok) {
        if (entries)
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (entries)
        alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
    *out = hu_tool_result_ok_owned(ok, 15);
    return HU_OK;
#endif
}

static const char *memory_recall_name(void *ctx) {
    (void)ctx;
    return HU_MEMORY_RECALL_NAME;
}
static const char *memory_recall_description(void *ctx) {
    (void)ctx;
    return HU_MEMORY_RECALL_DESC;
}
static const char *memory_recall_parameters_json(void *ctx) {
    (void)ctx;
    return HU_MEMORY_RECALL_PARAMS;
}
static void memory_recall_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(hu_memory_recall_ctx_t));
}

static const hu_tool_vtable_t memory_recall_vtable = {
    .execute = memory_recall_execute,
    .name = memory_recall_name,
    .description = memory_recall_description,
    .parameters_json = memory_recall_parameters_json,
    .deinit = memory_recall_deinit,
};

hu_error_t hu_memory_recall_create(hu_allocator_t *alloc, hu_memory_t *memory, hu_tool_t *out) {
    hu_memory_recall_ctx_t *c = (hu_memory_recall_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->memory = memory;
    out->ctx = c;
    out->vtable = &memory_recall_vtable;
    return HU_OK;
}
