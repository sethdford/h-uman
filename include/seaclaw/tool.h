#ifndef SC_TOOL_H
#define SC_TOOL_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/json.h"
#include "core/slice.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Tool types — execution result, spec
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_tool_result {
    bool success;
    const char *output;
    size_t output_len;
    const char *error_msg;
    size_t error_msg_len;
    bool output_owned;    /* true = caller must free output; false = static/borrowed */
    bool error_msg_owned; /* true = caller must free error_msg */
    bool needs_approval;  /* true = tool needs user approval to proceed */
} sc_tool_result_t;

static inline sc_tool_result_t sc_tool_result_ok(const char *output, size_t len) {
    return (sc_tool_result_t){
        .success = true,
        .output = output,
        .output_len = len,
        .error_msg = NULL,
        .error_msg_len = 0,
        .output_owned = false,
        .error_msg_owned = false,
    };
}

static inline sc_tool_result_t sc_tool_result_ok_owned(const char *output, size_t len) {
    return (sc_tool_result_t){
        .success = true,
        .output = output,
        .output_len = len,
        .error_msg = NULL,
        .error_msg_len = 0,
        .output_owned = true,
        .error_msg_owned = false,
    };
}

static inline sc_tool_result_t sc_tool_result_fail(const char *error_msg, size_t len) {
    return (sc_tool_result_t){
        .success = false,
        .output = "",
        .output_len = 0,
        .error_msg = error_msg,
        .error_msg_len = len,
        .output_owned = false,
        .error_msg_owned = false,
    };
}

static inline sc_tool_result_t sc_tool_result_fail_owned(const char *error_msg, size_t len) {
    return (sc_tool_result_t){
        .success = false,
        .output = "",
        .output_len = 0,
        .error_msg = error_msg,
        .error_msg_len = len,
        .output_owned = false,
        .error_msg_owned = true,
    };
}

static inline void sc_tool_result_free(sc_allocator_t *alloc, sc_tool_result_t *r) {
    if (r->output_owned && r->output && r->output_len > 0)
        alloc->free(alloc->ctx, (void *)r->output, r->output_len + 1);
    if (r->error_msg_owned && r->error_msg && r->error_msg_len > 0)
        alloc->free(alloc->ctx, (void *)r->error_msg, r->error_msg_len + 1);
    r->output = NULL;
    r->error_msg = NULL;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Tool vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct sc_tool_vtable;

typedef struct sc_tool {
    void *ctx;
    const struct sc_tool_vtable *vtable;
} sc_tool_t;

/* args: JSON object (sc_json_value_t with type SC_JSON_OBJECT) */
typedef struct sc_tool_vtable {
    sc_error_t (*execute)(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                          sc_tool_result_t *out);
    const char *(*name)(void *ctx);
    const char *(*description)(void *ctx);
    const char *(*parameters_json)(void *ctx);

    /* Optional — may be NULL. Clean up heap-allocated tool struct. */
    void (*deinit)(void *ctx, sc_allocator_t *alloc);
} sc_tool_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * SC_TOOL_IMPL macro — quick vtable definition
 * Usage:
 *   static const sc_tool_vtable_t my_tool_vtable = {
 *       .execute = my_tool_execute,
 *       .name = my_tool_name,
 *       .description = my_tool_description,
 *       .parameters_json = my_tool_parameters_json,
 *       .deinit = my_tool_deinit,
 *   };
 * Or use SC_TOOL_IMPL(MyTool, execute_fn, name_fn, desc_fn, params_fn, deinit_fn)
 * ────────────────────────────────────────────────────────────────────────── */

#define SC_TOOL_IMPL(Prefix, execute_fn, name_fn, desc_fn, params_fn, deinit_fn) \
    static const sc_tool_vtable_t Prefix##_vtable = {                            \
        .execute = execute_fn,                                                   \
        .name = name_fn,                                                         \
        .description = desc_fn,                                                  \
        .parameters_json = params_fn,                                            \
        .deinit = deinit_fn,                                                     \
    }

#endif /* SC_TOOL_H */
