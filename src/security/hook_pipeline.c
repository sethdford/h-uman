/* Hook pipeline: orchestrated execution of hook entries */
#include "human/core/log.h"
#include "human/hook_pipeline.h"
#include "human/hook.h"
#include <stdio.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Convert exit code to hook decision
 * ────────────────────────────────────────────────────────────────────────── */

static hu_hook_decision_t hu_hook_exit_code_to_decision(int exit_code) {
    switch (exit_code) {
    case 0:
        return HU_HOOK_ALLOW;
    case 2:
        return HU_HOOK_DENY;
    case 3:
        return HU_HOOK_WARN;
    default:
        /* Unknown exit code: treat as allow with warning */
        return HU_HOOK_ALLOW;
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Pipeline execution
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_hook_pipeline_execute(const hu_hook_registry_t *registry,
                                    hu_allocator_t *alloc,
                                    const hu_hook_context_t *ctx,
                                    hu_hook_result_t *result) {
    if (!alloc || !ctx || !result)
        return HU_ERR_INVALID_ARGUMENT;

    /* Default to allow */
    result->decision = HU_HOOK_ALLOW;
    result->message = NULL;
    result->message_len = 0;

    if (!registry)
        return HU_OK;

    size_t count = hu_hook_registry_count(registry);
    bool had_warn = false;

    for (size_t i = 0; i < count; i++) {
        const hu_hook_entry_t *entry = hu_hook_registry_get(registry, i);
        if (!entry)
            continue;

        /* Only run hooks matching the event type */
        if (entry->event != ctx->event)
            continue;

        int exit_code = -1;
        char *stdout_buf = NULL;
        size_t stdout_len = 0;

        hu_error_t err = hu_hook_shell_execute(alloc, entry, ctx, &exit_code,
                                               &stdout_buf, &stdout_len);

        if (err != HU_OK) {
            /* Execution error */
            if (entry->required) {
                /* Required hook failed to execute => deny */
                result->decision = HU_HOOK_DENY;
                const char *msg = "required hook failed to execute";
                size_t msg_len = strlen(msg);
                result->message = alloc->alloc(alloc->ctx, msg_len + 1);
                if (result->message) {
                    memcpy(result->message, msg, msg_len + 1);
                    result->message_len = msg_len;
                }
                if (stdout_buf)
                    alloc->free(alloc->ctx, stdout_buf, stdout_len + 1);
                return HU_OK;
            }
            /* Non-required: log and continue */
            hu_log_error("hook_pipeline", NULL, "hook '%.*s' execution error: %d (continuing)",
                    (int)(entry->name_len ? entry->name_len : 7),
                    entry->name ? entry->name : "unnamed",
                    (int)err);
            if (stdout_buf)
                alloc->free(alloc->ctx, stdout_buf, stdout_len + 1);
            continue;
        }

        hu_hook_decision_t decision = hu_hook_exit_code_to_decision(exit_code);

        /* Handle unknown exit codes */
        if (exit_code != 0 && exit_code != 2 && exit_code != 3) {
            hu_log_info("hook_pipeline", NULL, "hook '%.*s' returned unexpected exit code %d "
                    "(treating as allow)",
                    (int)(entry->name_len ? entry->name_len : 7),
                    entry->name ? entry->name : "unnamed",
                    exit_code);
            if (entry->required) {
                /* Required hook with unknown exit code => deny for safety */
                decision = HU_HOOK_DENY;
            }
        }

        if (decision == HU_HOOK_DENY) {
            result->decision = HU_HOOK_DENY;
            /* Capture stdout as the deny message */
            if (stdout_buf && stdout_len > 0) {
                result->message = stdout_buf;
                result->message_len = stdout_len;
            } else {
                if (stdout_buf)
                    alloc->free(alloc->ctx, stdout_buf, stdout_len + 1);
                const char *deny_msg = "denied by hook";
                size_t deny_len = 14;
                result->message = alloc->alloc(alloc->ctx, deny_len + 1);
                if (result->message) {
                    memcpy(result->message, deny_msg, deny_len + 1);
                    result->message_len = deny_len;
                }
            }
            return HU_OK; /* First deny stops pipeline */
        }

        if (decision == HU_HOOK_WARN) {
            had_warn = true;
            hu_log_error("hook_pipeline", NULL, "hook '%.*s' warns: %.*s",
                    (int)(entry->name_len ? entry->name_len : 7),
                    entry->name ? entry->name : "unnamed",
                    (int)(stdout_len > 200 ? 200 : stdout_len),
                    stdout_buf ? stdout_buf : "(no output)");
        }

        if (stdout_buf)
            alloc->free(alloc->ctx, stdout_buf, stdout_len + 1);
    }

    if (had_warn)
        result->decision = HU_HOOK_WARN;

    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Convenience wrappers
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_hook_pipeline_pre_tool(const hu_hook_registry_t *registry,
                                     hu_allocator_t *alloc,
                                     const char *tool_name, size_t tool_name_len,
                                     const char *args_json, size_t args_json_len,
                                     hu_hook_result_t *result) {
    hu_hook_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.event = HU_HOOK_PRE_TOOL_EXECUTE;
    ctx.tool_name = tool_name;
    ctx.tool_name_len = tool_name_len;
    ctx.args_json = args_json;
    ctx.args_json_len = args_json_len;
    return hu_hook_pipeline_execute(registry, alloc, &ctx, result);
}

hu_error_t hu_hook_pipeline_post_tool(const hu_hook_registry_t *registry,
                                      hu_allocator_t *alloc,
                                      const char *tool_name, size_t tool_name_len,
                                      const char *args_json, size_t args_json_len,
                                      const char *result_output, size_t result_output_len,
                                      bool result_success,
                                      hu_hook_result_t *result) {
    hu_hook_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.event = HU_HOOK_POST_TOOL_EXECUTE;
    ctx.tool_name = tool_name;
    ctx.tool_name_len = tool_name_len;
    ctx.args_json = args_json;
    ctx.args_json_len = args_json_len;
    ctx.result_output = result_output;
    ctx.result_output_len = result_output_len;
    ctx.result_success = result_success;
    return hu_hook_pipeline_execute(registry, alloc, &ctx, result);
}
