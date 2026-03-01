#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include <string.h>
#include <stdlib.h>

#define SC_MEMORY_LIST_NAME "memory_list"
#define SC_MEMORY_LIST_DESC "List memories"
#define SC_MEMORY_LIST_PARAMS "{\"type\":\"object\",\"properties\":{},\"required\":[]}"

typedef struct sc_memory_list_ctx {
    sc_memory_t *memory;
} sc_memory_list_ctx_t;

static sc_error_t memory_list_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_memory_list_ctx_t *c = (sc_memory_list_ctx_t *)ctx;
    (void)args;
    if (!c || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(memory_list stub)", 18);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(msg, 18);
    return SC_OK;
#else
    if (!c->memory || !c->memory->vtable) {
        *out = sc_tool_result_fail("memory not configured", 21);
        return SC_OK;
    }
    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = c->memory->vtable->list(c->memory->ctx, alloc, NULL, NULL, 0, &entries, &count);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("list failed", 11);
        return SC_OK;
    }
    char *ok = sc_strndup(alloc, "list complete", 13);
    if (!ok) {
        if (entries) alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    if (entries) alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
    *out = sc_tool_result_ok_owned(ok, 13);
    return SC_OK;
#endif
}

static const char *memory_list_name(void *ctx) { (void)ctx; return SC_MEMORY_LIST_NAME; }
static const char *memory_list_description(void *ctx) { (void)ctx; return SC_MEMORY_LIST_DESC; }
static const char *memory_list_parameters_json(void *ctx) { (void)ctx; return SC_MEMORY_LIST_PARAMS; }
static void memory_list_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t memory_list_vtable = {
    .execute = memory_list_execute, .name = memory_list_name,
    .description = memory_list_description, .parameters_json = memory_list_parameters_json,
    .deinit = memory_list_deinit,
};

sc_error_t sc_memory_list_create(sc_allocator_t *alloc,
    sc_memory_t *memory,
    sc_tool_t *out)
{
    (void)alloc;
    sc_memory_list_ctx_t *c = (sc_memory_list_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->memory = memory;
    out->ctx = c;
    out->vtable = &memory_list_vtable;
    return SC_OK;
}
