#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include <string.h>
#include <stdlib.h>

#define SC_MEMORY_FORGET_NAME "memory_forget"
#define SC_MEMORY_FORGET_DESC "Forget memory by key"
#define SC_MEMORY_FORGET_PARAMS "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}"
#define SC_MEMORY_KEY_MAX 1024

typedef struct sc_memory_forget_ctx {
    sc_memory_t *memory;
} sc_memory_forget_ctx_t;

static sc_error_t memory_forget_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_memory_forget_ctx_t *c = (sc_memory_forget_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *key = sc_json_get_string(args, "key");
    if (!key || strlen(key) == 0) {
        *out = sc_tool_result_fail("missing key", 11);
        return SC_OK;
    }
    if (strlen(key) > SC_MEMORY_KEY_MAX) {
        *out = sc_tool_result_fail("key too long", 12);
        return SC_OK;
    }
#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(memory_forget stub)", 20);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(msg, 20);
    return SC_OK;
#else
    if (!c->memory || !c->memory->vtable) {
        *out = sc_tool_result_fail("memory not configured", 21);
        return SC_OK;
    }
    bool deleted = false;
    sc_error_t err = c->memory->vtable->forget(c->memory->ctx, key, strlen(key), &deleted);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("forget failed", 13);
        return SC_OK;
    }
    const char *msg = deleted ? "forgotten" : "not found";
    size_t msg_len = strlen(msg);
    char *ok = sc_strndup(alloc, msg, msg_len);
    if (!ok) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(ok, msg_len);
    return SC_OK;
#endif
}

static const char *memory_forget_name(void *ctx) { (void)ctx; return SC_MEMORY_FORGET_NAME; }
static const char *memory_forget_description(void *ctx) { (void)ctx; return SC_MEMORY_FORGET_DESC; }
static const char *memory_forget_parameters_json(void *ctx) { (void)ctx; return SC_MEMORY_FORGET_PARAMS; }
static void memory_forget_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t memory_forget_vtable = {
    .execute = memory_forget_execute, .name = memory_forget_name,
    .description = memory_forget_description, .parameters_json = memory_forget_parameters_json,
    .deinit = memory_forget_deinit,
};

sc_error_t sc_memory_forget_create(sc_allocator_t *alloc,
    sc_memory_t *memory,
    sc_tool_t *out)
{
    (void)alloc;
    sc_memory_forget_ctx_t *c = (sc_memory_forget_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->memory = memory;
    out->ctx = c;
    out->vtable = &memory_forget_vtable;
    return SC_OK;
}
