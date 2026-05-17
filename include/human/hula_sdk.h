#ifndef HU_HULA_SDK_H
#define HU_HULA_SDK_H

/*
 * HuLa SDK — Public API for the Human Language intermediate representation.
 *
 * HuLa (Human Language) is a typed IR for orchestrating AI agent programs.
 * It provides nine opcodes that map to tool execution, control flow, and
 * multi-agent coordination.
 *
 * This header is the **stable SDK surface** for embedders: it re-exports the
 * implementation in `human/agent/hula.h` and adds version macros plus small
 * helpers for common tasks. Breaking changes to these helpers bump
 * `HU_HULA_SDK_VERSION_MAJOR`; the underlying IR version remains
 * `HU_HULA_VERSION` in `hula.h`.
 *
 * Quick start:
 *   1. Parse a HuLa program from JSON: hu_hula_parse_json()
 *   2. Validate it: hu_hula_validate()
 *   3. Create an executor: hu_hula_exec_init() or hu_hula_exec_init_full()
 *   4. Run it: hu_hula_exec_run()
 *   5. Inspect results: hu_hula_exec_result() (by node `id`) or slots / trace
 *
 * To compile natural language into IR, use the LLM compiler layer declared in
 * `human/agent/hula_compiler.h` (for example hu_hula_compiler_parse_response()).
 *
 * Wire format: JSON. Programs are serializable and can be stored or transmitted.
 * In JSON, tools are keyed as `"tool"`; in C structs the field is `tool_name`.
 * EMIT nodes use `"emit_key"` / `"emit_value"` in JSON (`emit_key` / `emit_value`
 * on `hu_hula_node_t`).
 *
 * Opcodes:
 *   CALL     - Invoke a registered tool with JSON arguments
 *   SEQ      - Execute children sequentially (short-circuit on failure)
 *   PAR      - Execute children concurrently (join-all)
 *   BRANCH   - Conditional: if predicate matches, then/else
 *   LOOP     - Repeat while predicate holds (bounded)
 *   DELEGATE - Spawn a sub-agent with a goal string
 *   EMIT     - Produce a named output value
 *   TRY      - Run body; on failure, run optional catch
 *   VERIFY   - Validate output against predicate; halt on mismatch
 *
 * Example JSON program:
 *   {
 *     "version": 1,
 *     "name": "greet-and-search",
 *     "root": {
 *       "op": "seq",
 *       "id": "s1",
 *       "children": [
 *         {"op": "call", "id": "c1", "tool": "web_search",
 *          "args": {"query": "weather today"}},
 *         {"op": "emit", "id": "e1", "emit_key": "greeting",
 *          "emit_value": "Good morning! {{result}}"}
 *       ]
 *     }
 *   }
 */

#include "human/agent/hula.h"
#include "human/core/string.h"

#include <stdio.h>
#include <string.h>

/* SDK version — follows semver. Bump MAJOR for breaking changes to this file. */
#define HU_HULA_SDK_VERSION_MAJOR  0
#define HU_HULA_SDK_VERSION_MINOR  2
#define HU_HULA_SDK_VERSION_PATCH  0
#define HU_HULA_SDK_VERSION_STRING "0.2.0"

/* ── v0.2.0 binding-friendly surface (additive) ─────────────────────────── */

/**
 * Opaque per-binding context handle.
 *
 * Bindings (Python ctypes / TypeScript N-API / etc.) instantiate one of
 * these per "HulaContext"-style object and attach a finalizer that calls
 * hu_hula_ctx_destroy(). The internal layout is intentionally hidden so it
 * can evolve in v0.3+ without breaking the FFI ABI: bindings only ever see
 * the typedef and pass `hu_hula_ctx_t *` opaquely.
 *
 * Stable in v0.2.x. Changes to this typedef or to the functions below bump
 * HU_HULA_SDK_VERSION_MAJOR.
 *
 * Not thread-safe in v0.2.0 — concurrent use of a single ctx from multiple
 * threads is undefined behavior. Out of scope per the v0.2.0 story; bindings
 * that need concurrency should create one ctx per thread.
 */
typedef struct hu_hula_ctx hu_hula_ctx_t;

/**
 * Allocate and initialize a new HuLa SDK context.
 *
 * The context stores a copy of `*alloc` by value; the allocator vtable
 * itself need not outlive the context, but `alloc->ctx` (if any) must
 * remain valid until hu_hula_ctx_destroy() returns.
 *
 * Returns HU_OK on success and writes the new handle into `*out`.
 * On HU_ERR_INVALID_ARGUMENT (NULL `alloc` or NULL `out`) and on
 * HU_ERR_OUT_OF_MEMORY, `*out` is left unmodified — callers may pre-seed
 * `*out` with a sentinel and trust it survives.
 *
 * Stable in v0.2.x.
 */
hu_error_t hu_hula_ctx_create(hu_allocator_t *alloc, hu_hula_ctx_t **out);

/**
 * Destroy a context allocated by hu_hula_ctx_create().
 *
 * NULL-safe: passing NULL is a no-op (binding finalizers may run twice on
 * a torn-down object, and that must not crash).
 *
 * Stable in v0.2.x.
 */
void hu_hula_ctx_destroy(hu_hula_ctx_t *ctx);

/**
 * Map an `hu_error_t` value to a human-readable, NUL-terminated ASCII string.
 *
 * For known codes (HU_OK and every HU_ERR_* declared in
 * `human/core/error.h`) returns the C identifier as a string literal, e.g.
 * "HU_ERR_INVALID_ARGUMENT". Distinct codes return distinct strings.
 *
 * For any value not covered by the switch (future codes, garbage, deliberate
 * fuzz input) returns the sentinel literal "HU_ERR_UNKNOWN".
 *
 * The returned pointer is to a static string literal and remains valid for
 * the lifetime of the program. Callers must not free it. Never NULL.
 *
 * Bindings render exception messages via this function. Implemented as an
 * extern symbol (not `static inline`) so FFI/ctypes/N-API have a single
 * dlsym target.
 *
 * Stable in v0.2.x.
 */
const char *hu_hula_error_string(hu_error_t err);

/* ── Ergonomic helpers (v0.1.0 — unchanged) ─────────────────────────────── */

/**
 * Build a minimal program whose root is a single CALL node.
 *
 * `tool_name` and `args_json` are copied into the program allocator; they need
 * not outlive the call. If `args_json` is NULL, it defaults to `"{}"`.
 *
 * On success, caller must call hu_hula_program_deinit() on `out`.
 */
static inline hu_error_t hu_hula_sdk_call(hu_allocator_t *alloc, const char *tool_name,
                                          const char *args_json, hu_hula_program_t *out) {
    if (!alloc || !tool_name || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_hula_program_init(out, *alloc, "sdk_call", 8);
    if (err != HU_OK)
        return err;

    hu_hula_node_t *root = hu_hula_program_alloc_node(out, HU_HULA_CALL, "root");
    if (!root) {
        hu_hula_program_deinit(out);
        return HU_ERR_OUT_OF_MEMORY;
    }

    root->tool_name = hu_strdup(&out->alloc, tool_name);
    if (!root->tool_name) {
        hu_hula_program_deinit(out);
        return HU_ERR_OUT_OF_MEMORY;
    }

    const char *args = args_json ? args_json : "{}";
    root->args_json = hu_strdup(&out->alloc, args);
    if (!root->args_json) {
        hu_hula_program_deinit(out);
        return HU_ERR_OUT_OF_MEMORY;
    }

    out->root = root;
    return HU_OK;
}

/**
 * Build a SEQ program with `count` CALL children (in order).
 *
 * Parallel arrays `tool_names` and `args_jsons` must have length `count`.
 * Each string is copied into the program allocator. NULL `args_jsons[i]`
 * becomes `"{}"`. Node ids are `c0` … `c{n}` (stable for hu_hula_exec_result).
 *
 * On success, caller must call hu_hula_program_deinit() on `out`.
 */
static inline hu_error_t hu_hula_sdk_sequence(hu_allocator_t *alloc, const char **tool_names,
                                              const char **args_jsons, size_t count,
                                              hu_hula_program_t *out) {
    if (!alloc || !tool_names || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (count == 0 || count > HU_HULA_MAX_CHILDREN)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_hula_program_init(out, *alloc, "sdk_seq", 7);
    if (err != HU_OK)
        return err;

    hu_hula_node_t *seq = hu_hula_program_alloc_node(out, HU_HULA_SEQ, "seq");
    if (!seq) {
        hu_hula_program_deinit(out);
        return HU_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < count; i++) {
        char idbuf[16];
        (void)snprintf(idbuf, sizeof(idbuf), "c%zu", i);

        hu_hula_node_t *call = hu_hula_program_alloc_node(out, HU_HULA_CALL, idbuf);
        if (!call) {
            hu_hula_program_deinit(out);
            return HU_ERR_OUT_OF_MEMORY;
        }

        call->tool_name = hu_strdup(&out->alloc, tool_names[i]);
        if (!call->tool_name) {
            hu_hula_program_deinit(out);
            return HU_ERR_OUT_OF_MEMORY;
        }

        const char *aj = (args_jsons && args_jsons[i]) ? args_jsons[i] : "{}";
        call->args_json = hu_strdup(&out->alloc, aj);
        if (!call->args_json) {
            hu_hula_program_deinit(out);
            return HU_ERR_OUT_OF_MEMORY;
        }

        seq->children[seq->children_count++] = call;
    }

    out->root = seq;
    return HU_OK;
}

/**
 * Parse JSON, validate against `tools`, execute, and copy the root node's output.
 *
 * Tool names for validation are taken from each `hu_tool_t` vtable's `name()`.
 * If `tools_count` is zero, validation skips the tool registry check (structure
 * and opcode rules still apply). When `policy` or `observer` is non-NULL,
 * hu_hula_exec_init_full() is used so policy and tracing integrate; otherwise
 * hu_hula_exec_init() is used.
 *
 * On `HU_OK`, if the root result has output, `*result_out` is allocated with
 * `alloc` and is NUL-terminated (length in `*result_len_out` excludes the
 * terminator). Caller must free with `alloc->free`. If there is no root id or
 * no output, `*result_out` may be NULL.
 */
static inline hu_error_t hu_hula_sdk_run_json(hu_allocator_t *alloc, const char *json,
                                              size_t json_len, hu_tool_t *tools, size_t tools_count,
                                              hu_security_policy_t *policy, hu_observer_t *observer,
                                              char **result_out, size_t *result_len_out) {
    if (!alloc || !json || !result_out)
        return HU_ERR_INVALID_ARGUMENT;

    *result_out = NULL;
    if (result_len_out)
        *result_len_out = 0;

    hu_hula_program_t prog;
    memset(&prog, 0, sizeof(prog));

    hu_error_t err = hu_hula_parse_json(alloc, json, json_len, &prog);
    if (err != HU_OK)
        return err;

    const char **tool_names = NULL;
    if (tools_count > 0) {
        if (!tools) {
            hu_hula_program_deinit(&prog);
            return HU_ERR_INVALID_ARGUMENT;
        }
        tool_names = (const char **)alloc->alloc(alloc->ctx, tools_count * sizeof(*tool_names));
        if (!tool_names) {
            hu_hula_program_deinit(&prog);
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < tools_count; i++) {
            tool_names[i] = (tools[i].vtable && tools[i].vtable->name)
                                ? tools[i].vtable->name(tools[i].ctx)
                                : "";
        }
    }

    hu_hula_validation_t val;
    memset(&val, 0, sizeof(val));
    err = hu_hula_validate(&prog, alloc, tool_names, tools_count, &val);
    if (tool_names)
        alloc->free(alloc->ctx, tool_names, tools_count * sizeof(*tool_names));
    if (err != HU_OK) {
        hu_hula_program_deinit(&prog);
        return err;
    }
    if (!val.valid) {
        hu_hula_validation_deinit(alloc, &val);
        hu_hula_program_deinit(&prog);
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_hula_validation_deinit(alloc, &val);

    hu_hula_exec_t exec;
    if (policy || observer) {
        err = hu_hula_exec_init_full(&exec, *alloc, &prog, tools, tools_count, policy, observer);
    } else {
        err = hu_hula_exec_init(&exec, *alloc, &prog, tools, tools_count);
    }
    if (err != HU_OK) {
        hu_hula_program_deinit(&prog);
        return err;
    }

    err = hu_hula_exec_run(&exec);
    if (err == HU_OK) {
        const char *root_id = prog.root && prog.root->id ? prog.root->id : NULL;
        const hu_hula_result_t *rr = root_id ? hu_hula_exec_result(&exec, root_id) : NULL;
        if (rr && rr->output && rr->output_len > 0) {
            *result_out = (char *)alloc->alloc(alloc->ctx, rr->output_len + 1);
            if (!*result_out) {
                err = HU_ERR_OUT_OF_MEMORY;
            } else {
                memcpy(*result_out, rr->output, rr->output_len);
                (*result_out)[rr->output_len] = '\0';
                if (result_len_out)
                    *result_len_out = rr->output_len;
            }
        } else if (rr && rr->output && rr->output_len == 0) {
            *result_out = (char *)alloc->alloc(alloc->ctx, 1);
            if (!*result_out) {
                err = HU_ERR_OUT_OF_MEMORY;
            } else {
                (*result_out)[0] = '\0';
                if (result_len_out)
                    *result_len_out = 0;
            }
        }
    }

    hu_hula_exec_deinit(&exec);
    hu_hula_program_deinit(&prog);
    return err;
}

#endif /* HU_HULA_SDK_H */
