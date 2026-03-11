#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/tool.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HU_SAVE_NAME "save_for_later"
#define HU_SAVE_DESC "Save content (URL, article, link) to share with contacts later"
#define HU_SAVE_PARAMS                                                                       \
    "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":"     \
    "\"URL or content to save\"},\"summary\":{\"type\":\"string\",\"description\":"          \
    "\"Brief summary of what this is about\"},\"topic\":{\"type\":\"string\",\"description\":" \
    "\"Topic category for matching to contacts\"}},\"required\":[\"url\",\"summary\"]}"

typedef struct {
    hu_memory_t *memory;
} hu_save_ctx_t;

static hu_error_t save_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                               hu_tool_result_t *out) {
    hu_save_ctx_t *c = (hu_save_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *url = hu_json_get_string(args, "url");
    const char *summary = hu_json_get_string(args, "summary");
    const char *topic = hu_json_get_string(args, "topic");
    if (!url || !url[0]) {
        *out = hu_tool_result_fail("missing required parameter: url", 31);
        return HU_OK;
    }
    if (!summary || !summary[0]) {
        *out = hu_tool_result_fail("missing required parameter: summary", 35);
        return HU_OK;
    }
    if (!topic || !topic[0])
        topic = "general";

    if (!c->memory || !c->memory->vtable || !c->memory->vtable->store) {
        *out = hu_tool_result_fail("memory not configured", 21);
        return HU_OK;
    }

    char key[512];
    int kn = snprintf(key, sizeof(key), "shareable::%s", url);
    if (kn <= 0 || (size_t)kn >= sizeof(key)) {
        *out = hu_tool_result_fail("url too long", 12);
        return HU_OK;
    }

    char content[2048];
    int cn = snprintf(content, sizeof(content), "[topic:%s] %s", topic, summary);
    if (cn <= 0 || (size_t)cn >= sizeof(content)) {
        *out = hu_tool_result_fail("content too long", 16);
        return HU_OK;
    }

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CUSTOM};
    const char *sid = c->memory->current_session_id;
    size_t sid_len = c->memory->current_session_id_len;
    hu_error_t err = c->memory->vtable->store(c->memory->ctx, key, (size_t)kn, content, (size_t)cn,
                                              &cat, sid, sid_len);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("failed to store content", 23);
        return HU_OK;
    }

    char *ok = hu_strndup(alloc, "saved for later", 15);
    if (!ok) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(ok, 15);
    return HU_OK;
}

static const char *save_name(void *ctx) { (void)ctx; return HU_SAVE_NAME; }
static const char *save_description(void *ctx) { (void)ctx; return HU_SAVE_DESC; }
static const char *save_parameters_json(void *ctx) { (void)ctx; return HU_SAVE_PARAMS; }
static void save_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(hu_save_ctx_t));
}

static const hu_tool_vtable_t save_vtable = {
    .execute = save_execute,
    .name = save_name,
    .description = save_description,
    .parameters_json = save_parameters_json,
    .deinit = save_deinit,
};

hu_error_t hu_save_for_later_create(hu_allocator_t *alloc, hu_memory_t *memory, hu_tool_t *out) {
    hu_save_ctx_t *c = (hu_save_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->memory = memory;
    out->ctx = c;
    out->vtable = &save_vtable;
    return HU_OK;
}
