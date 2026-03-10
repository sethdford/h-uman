#include "human/tools/claude_code.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/security.h"
#include "human/security/sandbox.h"
#include "human/tools/validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_GATEWAY_POSIX
#include <sys/wait.h>
#include <unistd.h>
#endif

#define HU_CLAUDE_CODE_NAME "claude_code"
#define HU_CLAUDE_CODE_DESC                                          \
    "Delegate a coding task to Claude Code (claude -p). "            \
    "Runs Claude CLI as a sub-agent with full coding capabilities. " \
    "Use for complex coding, refactoring, debugging, or multi-file changes."

#define HU_CLAUDE_CODE_PARAMS                      \
    "{\"type\":\"object\",\"properties\":{"        \
    "\"prompt\":{\"type\":\"string\"},"            \
    "\"working_directory\":{\"type\":\"string\"}," \
    "\"allowed_tools\":{\"type\":\"string\"},"     \
    "\"model\":{\"type\":\"string\"},"             \
    "\"max_turns\":{\"type\":\"integer\"},"        \
    "\"context\":{\"type\":\"string\"}"            \
    "},\"required\":[\"prompt\"]}"

typedef struct hu_claude_code_ctx {
    hu_allocator_t *alloc;
    hu_security_policy_t *policy;
} hu_claude_code_ctx_t;

#if defined(HU_GATEWAY_POSIX) && !HU_IS_TEST
static hu_error_t run_claude_code(hu_allocator_t *alloc, hu_security_policy_t *policy,
                                  const char *prompt, size_t prompt_len, const char *working_dir,
                                  const char *allowed_tools, const char *model, int max_turns,
                                  char **out, size_t *out_len) {
    char *argv[16];
    int argc = 0;
    argv[argc++] = "claude";
    argv[argc++] = "-p";
    argv[argc++] = "--output-format";
    argv[argc++] = "json";

    char model_buf[128];
    if (model && strlen(model) > 0) {
        argv[argc++] = "--model";
        size_t mlen = strlen(model);
        if (mlen >= sizeof(model_buf))
            mlen = sizeof(model_buf) - 1;
        memcpy(model_buf, model, mlen);
        model_buf[mlen] = '\0';
        argv[argc++] = model_buf;
    }

    char tools_buf[512];
    if (allowed_tools && strlen(allowed_tools) > 0) {
        argv[argc++] = "--allowedTools";
        size_t tlen = strlen(allowed_tools);
        if (tlen >= sizeof(tools_buf))
            tlen = sizeof(tools_buf) - 1;
        memcpy(tools_buf, allowed_tools, tlen);
        tools_buf[tlen] = '\0';
        argv[argc++] = tools_buf;
    }

    char turns_buf[16];
    if (max_turns > 0) {
        argv[argc++] = "--max-turns";
        snprintf(turns_buf, sizeof(turns_buf), "%d", max_turns);
        argv[argc++] = turns_buf;
    }

    argv[argc] = NULL;

    int stdout_fds[2], stdin_fds[2];
    if (pipe(stdout_fds) != 0)
        return HU_ERR_IO;
    if (pipe(stdin_fds) != 0) {
        close(stdout_fds[0]);
        close(stdout_fds[1]);
        return HU_ERR_IO;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_fds[0]);
        close(stdout_fds[1]);
        close(stdin_fds[0]);
        close(stdin_fds[1]);
        return HU_ERR_IO;
    }

    if (pid == 0) {
        close(stdout_fds[0]);
        close(stdin_fds[1]);
        dup2(stdin_fds[0], STDIN_FILENO);
        dup2(stdout_fds[1], STDOUT_FILENO);
        dup2(stdout_fds[1], STDERR_FILENO);
        close(stdin_fds[0]);
        close(stdout_fds[1]);

        if (working_dir && strlen(working_dir) > 0) {
            if (hu_tool_validate_path(working_dir, NULL, 0) != HU_OK)
                _exit(126);
            if (chdir(working_dir) != 0)
                _exit(126);
        }

        if (policy && policy->net_proxy && policy->net_proxy->enabled) {
            const char *addr = policy->net_proxy->proxy_addr;
            if (!addr)
                addr = "http://127.0.0.1:0";
            setenv("HTTP_PROXY", addr, 1);
            setenv("HTTPS_PROXY", addr, 1);
            setenv("http_proxy", addr, 1);
            setenv("https_proxy", addr, 1);
            if (policy->net_proxy->allowed_domains_count > 0) {
                size_t total = 0;
                for (size_t np = 0; np < policy->net_proxy->allowed_domains_count; np++) {
                    if (policy->net_proxy->allowed_domains[np])
                        total += strlen(policy->net_proxy->allowed_domains[np]) + 1;
                }
                if (total > 0) {
                    char *no_proxy = (char *)malloc(total + 1);
                    if (no_proxy) {
                        size_t off = 0;
                        for (size_t np = 0; np < policy->net_proxy->allowed_domains_count; np++) {
                            const char *d = policy->net_proxy->allowed_domains[np];
                            if (!d)
                                continue;
                            size_t dlen = strlen(d);
                            if (off > 0)
                                no_proxy[off++] = ',';
                            memcpy(no_proxy + off, d, dlen);
                            off += dlen;
                        }
                        no_proxy[off] = '\0';
                        setenv("NO_PROXY", no_proxy, 1);
                        setenv("no_proxy", no_proxy, 1);
                        free(no_proxy);
                    }
                }
            }
        }

        if (policy && policy->sandbox && policy->sandbox->vtable &&
            policy->sandbox->vtable->apply) {
            hu_error_t serr = policy->sandbox->vtable->apply(policy->sandbox->ctx);
            if (serr != HU_OK && serr != HU_ERR_NOT_SUPPORTED)
                _exit(125);
        }

        execvp("claude", argv);
        _exit(127);
    }

    close(stdout_fds[1]);
    close(stdin_fds[0]);

    size_t written = 0;
    while (written < prompt_len) {
        ssize_t w = write(stdin_fds[1], prompt + written, prompt_len - written);
        if (w <= 0)
            break;
        written += (size_t)w;
    }
    close(stdin_fds[1]);

    size_t cap = 512 * 1024;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        close(stdout_fds[0]);
        waitpid(pid, NULL, 0);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    for (;;) {
        if (len >= cap - 1) {
            size_t new_cap = cap * 2;
            if (new_cap > 8 * 1024 * 1024)
                break;
            char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nb)
                break;
            buf = nb;
            cap = new_cap;
        }
        ssize_t n = read(stdout_fds[0], buf + len, cap - len - 1);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    close(stdout_fds[0]);

    int status;
    waitpid(pid, &status, 0);

    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, buf, len, &parsed);
    if (err == HU_OK && parsed) {
        const char *result = hu_json_get_string(parsed, "result");
        if (result && strlen(result) > 0) {
            size_t rlen = strlen(result);
            alloc->free(alloc->ctx, buf, cap);
            *out = hu_strndup(alloc, result, rlen);
            *out_len = rlen;
            hu_json_free(alloc, parsed);
            return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
        }
        hu_json_free(alloc, parsed);
    }

    *out = buf;
    *out_len = len;
    return HU_OK;
}
#endif /* HU_GATEWAY_POSIX && !HU_IS_TEST */

static hu_error_t claude_code_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                      hu_tool_result_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *prompt = hu_json_get_string(args, "prompt");
    if (!prompt || strlen(prompt) == 0) {
        *out = hu_tool_result_fail("missing prompt", 14);
        return HU_OK;
    }

#if HU_IS_TEST
    (void)ctx;
    const char *working_dir = hu_json_get_string(args, "working_directory");
    const char *model = hu_json_get_string(args, "model");
    size_t need =
        64 + strlen(prompt) + (working_dir ? strlen(working_dir) : 0) + (model ? strlen(model) : 0);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, need + 1, "Claude Code [%s] in [%s]: %s", model ? model : "default",
                     working_dir ? working_dir : ".", prompt);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = hu_tool_result_ok_owned(msg, len);
    return HU_OK;
#elif defined(HU_GATEWAY_POSIX)
    {
        hu_claude_code_ctx_t *cc = (hu_claude_code_ctx_t *)ctx;
        const char *working_dir = hu_json_get_string(args, "working_directory");
        const char *allowed_tools = hu_json_get_string(args, "allowed_tools");
        const char *model = hu_json_get_string(args, "model");
        const char *context = hu_json_get_string(args, "context");

        int max_turns = 10;
        hu_json_value_t *turns_val = hu_json_object_get(args, "max_turns");
        if (turns_val && turns_val->type == HU_JSON_NUMBER)
            max_turns = (int)turns_val->data.number;

        char *full_prompt;
        size_t full_len;
        if (context && strlen(context) > 0) {
            size_t ctx_len = strlen(context);
            size_t prompt_len = strlen(prompt);
            full_len = ctx_len + 2 + prompt_len;
            full_prompt = (char *)alloc->alloc(alloc->ctx, full_len + 1);
            if (!full_prompt) {
                *out = hu_tool_result_fail("out of memory", 13);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(full_prompt, context, ctx_len);
            full_prompt[ctx_len] = '\n';
            full_prompt[ctx_len + 1] = '\n';
            memcpy(full_prompt + ctx_len + 2, prompt, prompt_len);
            full_prompt[full_len] = '\0';
        } else {
            full_len = strlen(prompt);
            full_prompt = hu_strndup(alloc, prompt, full_len);
            if (!full_prompt) {
                *out = hu_tool_result_fail("out of memory", 13);
                return HU_ERR_OUT_OF_MEMORY;
            }
        }

        char *result = NULL;
        size_t result_len = 0;
        hu_error_t err =
            run_claude_code(alloc, cc ? cc->policy : NULL, full_prompt, full_len, working_dir,
                            allowed_tools, model, max_turns, &result, &result_len);
        alloc->free(alloc->ctx, full_prompt, full_len + 1);

        if (err != HU_OK) {
            *out = hu_tool_result_fail("Claude Code execution failed", 28);
            return HU_OK;
        }

        *out = hu_tool_result_ok_owned(result, result_len);
        return HU_OK;
    }
#else
    (void)ctx;
    (void)alloc;
    *out = hu_tool_result_fail("Claude Code requires POSIX", 26);
    return HU_OK;
#endif
}

static const char *claude_code_name(void *ctx) {
    (void)ctx;
    return HU_CLAUDE_CODE_NAME;
}
static const char *claude_code_description(void *ctx) {
    (void)ctx;
    return HU_CLAUDE_CODE_DESC;
}
static const char *claude_code_parameters_json(void *ctx) {
    (void)ctx;
    return HU_CLAUDE_CODE_PARAMS;
}

static void claude_code_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(hu_claude_code_ctx_t));
}

static const hu_tool_vtable_t claude_code_vtable = {
    .execute = claude_code_execute,
    .name = claude_code_name,
    .description = claude_code_description,
    .parameters_json = claude_code_parameters_json,
    .deinit = claude_code_deinit,
};

hu_error_t hu_claude_code_create(hu_allocator_t *alloc, hu_security_policy_t *policy,
                                 hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_claude_code_ctx_t *c = (hu_claude_code_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    c->policy = policy;
    out->ctx = c;
    out->vtable = &claude_code_vtable;
    return HU_OK;
}
