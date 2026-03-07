#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include "seaclaw/tool.h"
#include <stdlib.h>
#include <string.h>

#define SC_MEMORY_RECALL_NAME "memory_recall"
#define SC_MEMORY_RECALL_DESC "Recall from memory"
#define SC_MEMORY_RECALL_PARAMS                                                                 \
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"limit\":{\"type\":" \
    "\"number\"}},\"required\":[\"query\"]}"
#define SC_MEMORY_QUERY_MAX 4096
#define SC_MEMORY_LIMIT_MAX 100

typedef struct sc_memory_recall_ctx {
    sc_memory_t *memory;
} sc_memory_recall_ctx_t;

static sc_error_t memory_recall_execute(void *ctx, sc_allocator_t *alloc,
                                        const sc_json_value_t *args, sc_tool_result_t *out) {
    sc_memory_recall_ctx_t *c = (sc_memory_recall_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *query = sc_json_get_string(args, "query");
    if (!query || strlen(query) == 0) {
        *out = sc_tool_result_fail("missing query", 13);
        return SC_OK;
    }
    if (strlen(query) > SC_MEMORY_QUERY_MAX) {
        *out = sc_tool_result_fail("query too long", 14);
        return SC_OK;
    }
#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(memory_recall stub)", 20);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, 20);
    return SC_OK;
#else
    if (!c->memory || !c->memory->vtable) {
        *out = sc_tool_result_fail("memory not configured", 21);
        return SC_OK;
    }
    double limit_val = sc_json_get_number(args, "limit", 10);
    if (limit_val < 0 || limit_val > SC_MEMORY_LIMIT_MAX)
        limit_val = 10;
    size_t limit = (size_t)limit_val;
    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    const char *sid = c->memory->current_session_id;
    size_t sid_len = c->memory->current_session_id_len;
    sc_error_t err = c->memory->vtable->recall(c->memory->ctx, alloc, query, strlen(query), limit,
                                               sid, sid_len, &entries, &count);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("recall failed", 13);
        return SC_OK;
    }
    char *ok = sc_strndup(alloc, "recall complete", 15);
    if (!ok) {
        if (entries)
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    if (entries)
        alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
    *out = sc_tool_result_ok_owned(ok, 15);
    return SC_OK;
#endif
}

static const char *memory_recall_name(void *ctx) {
    (void)ctx;
    return SC_MEMORY_RECALL_NAME;
}
static const char *memory_recall_description(void *ctx) {
    (void)ctx;
    return SC_MEMORY_RECALL_DESC;
}
static const char *memory_recall_parameters_json(void *ctx) {
    (void)ctx;
    return SC_MEMORY_RECALL_PARAMS;
}
static void memory_recall_deinit(void *ctx, sc_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(sc_memory_recall_ctx_t));
}

static const sc_tool_vtable_t memory_recall_vtable = {
    .execute = memory_recall_execute,
    .name = memory_recall_name,
    .description = memory_recall_description,
    .parameters_json = memory_recall_parameters_json,
    .deinit = memory_recall_deinit,
};

sc_error_t sc_memory_recall_create(sc_allocator_t *alloc, sc_memory_t *memory, sc_tool_t *out) {
    sc_memory_recall_ctx_t *c = (sc_memory_recall_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->memory = memory;
    out->ctx = c;
    out->vtable = &memory_recall_vtable;
    return SC_OK;
}
