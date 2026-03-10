#ifndef HU_TOOL_H
#define HU_TOOL_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/json.h"
#include "core/slice.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Tool types — execution result, spec
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_tool_result {
    bool success;
    const char *output;
    size_t output_len;
    const char *error_msg;
    size_t error_msg_len;
    bool output_owned;    /* true = caller must free output; false = static/borrowed */
    bool error_msg_owned; /* true = caller must free error_msg */
    bool needs_approval;  /* true = tool needs user approval to proceed */
} hu_tool_result_t;

static inline hu_tool_result_t hu_tool_result_ok(const char *output, size_t len) {
    return (hu_tool_result_t){
        .success = true,
        .output = output,
        .output_len = len,
        .error_msg = NULL,
        .error_msg_len = 0,
        .output_owned = false,
        .error_msg_owned = false,
    };
}

static inline hu_tool_result_t hu_tool_result_ok_owned(const char *output, size_t len) {
    return (hu_tool_result_t){
        .success = true,
        .output = output,
        .output_len = len,
        .error_msg = NULL,
        .error_msg_len = 0,
        .output_owned = true,
        .error_msg_owned = false,
    };
}

static inline hu_tool_result_t hu_tool_result_fail(const char *error_msg, size_t len) {
    return (hu_tool_result_t){
        .success = false,
        .output = "",
        .output_len = 0,
        .error_msg = error_msg,
        .error_msg_len = len,
        .output_owned = false,
        .error_msg_owned = false,
    };
}

static inline hu_tool_result_t hu_tool_result_fail_owned(const char *error_msg, size_t len) {
    return (hu_tool_result_t){
        .success = false,
        .output = "",
        .output_len = 0,
        .error_msg = error_msg,
        .error_msg_len = len,
        .output_owned = false,
        .error_msg_owned = true,
    };
}

static inline void hu_tool_result_free(hu_allocator_t *alloc, hu_tool_result_t *r) {
    if (r->output_owned && r->output)
        alloc->free(alloc->ctx, (void *)r->output, r->output_len + 1);
    if (r->error_msg_owned && r->error_msg)
        alloc->free(alloc->ctx, (void *)r->error_msg, r->error_msg_len + 1);
    r->output = NULL;
    r->error_msg = NULL;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Tool vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct hu_tool_vtable;

typedef struct hu_tool {
    void *ctx;
    const struct hu_tool_vtable *vtable;
} hu_tool_t;

/* args: JSON object (hu_json_value_t with type HU_JSON_OBJECT) */
typedef struct hu_tool_vtable {
    /**
     * Execute the tool with the given JSON input.
     *
     * Always set *out and return HU_OK. Use hu_tool_result_ok*() for success,
     * hu_tool_result_fail*() for tool-level failure (validation, file not found,
     * command failed, etc.). Return HU_ERR_* only for infrastructure failures
     * (allocation, missing context) when the tool cannot meaningfully set *out.
     */
    hu_error_t (*execute)(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                          hu_tool_result_t *out);
    const char *(*name)(void *ctx);
    const char *(*description)(void *ctx);
    const char *(*parameters_json)(void *ctx);

    /* Optional — may be NULL. Clean up heap-allocated tool struct. */
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_tool_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * HU_TOOL_IMPL macro — quick vtable definition
 * Usage:
 *   static const hu_tool_vtable_t my_tool_vtable = {
 *       .execute = my_tool_execute,
 *       .name = my_tool_name,
 *       .description = my_tool_description,
 *       .parameters_json = my_tool_parameters_json,
 *       .deinit = my_tool_deinit,
 *   };
 * Or use HU_TOOL_IMPL(MyTool, execute_fn, name_fn, desc_fn, params_fn, deinit_fn)
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_TOOL_IMPL(Prefix, execute_fn, name_fn, desc_fn, params_fn, deinit_fn) \
    static const hu_tool_vtable_t Prefix##_vtable = {                            \
        .execute = execute_fn,                                                   \
        .name = name_fn,                                                         \
        .description = desc_fn,                                                  \
        .parameters_json = params_fn,                                            \
        .deinit = deinit_fn,                                                     \
    }

#endif /* HU_TOOL_H */
