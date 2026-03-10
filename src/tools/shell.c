#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/security.h"
#include "human/security/sandbox.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define HU_SHELL_NAME "shell"
#define HU_SHELL_DESC "Execute shell commands. Use with caution."
#define HU_SHELL_PARAMS                                                                      \
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[" \
    "\"command\"]}"
#define HU_SHELL_CMD_MAX 4096

typedef struct hu_shell_ctx {
    const char *workspace_dir;
    size_t workspace_dir_len;
    hu_security_policy_t *policy;
} hu_shell_ctx_t;

/*
 * SECURITY WARNING: This tool passes commands directly to /bin/sh -c.
 * Shell metacharacters in cmd are interpreted by the shell.
 * This tool should be restricted or disabled in high-assurance deployments.
 * Use hu_policy_validate_command and sandbox wrapping for mitigation.
 *
 * RUNTIME WRAP: The hu_runtime_vtable_t has an optional wrap_command method
 * (e.g. Docker runtime wraps as "docker run ..."). Shell tool does not yet
 * use it; integration left for later. Callers that have a runtime can invoke
 * wrap_command to wrap argv before exec.
 */
static hu_error_t shell_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                hu_tool_result_t *out) {
    hu_shell_ctx_t *s = (hu_shell_ctx_t *)ctx;
    if (!s || !args || !out) {
        *out = hu_tool_result_fail("invalid args", 13);
        return HU_ERR_INVALID_ARGUMENT;
    }

#if HU_IS_TEST
    {
        const char *stub = "(shell disabled in test mode)";
        size_t stub_len = strlen(stub);
        char *msg = hu_strndup(alloc, stub, stub_len);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_OK;
        }
        *out = hu_tool_result_ok_owned(msg, stub_len);
    }
    return HU_OK;
#else
#ifndef _WIN32
    if (s->policy && !hu_security_shell_allowed(s->policy)) {
        *out = hu_tool_result_fail("shell execution not allowed by policy", 38);
        return HU_OK;
    }

    const char *cmd = hu_json_get_string(args, "command");
    if (!cmd || strlen(cmd) == 0) {
        *out = hu_tool_result_fail("missing command", 15);
        return HU_OK;
    }
    size_t cmd_len = strlen(cmd);
    if (cmd_len > HU_SHELL_CMD_MAX) {
        *out = hu_tool_result_fail("command too long", 16);
        return HU_OK;
    }

    if (s->policy) {
        bool approved = s->policy->pre_approved;
        s->policy->pre_approved = false;
        hu_command_risk_level_t risk;
        hu_error_t perr = hu_policy_validate_command(s->policy, cmd, approved, &risk);
        if (perr == HU_ERR_SECURITY_APPROVAL_REQUIRED) {
            *out = hu_tool_result_fail("approval required", 17);
            out->needs_approval = true;
            return HU_OK;
        }
        if (perr != HU_OK) {
            *out = hu_tool_result_fail("command blocked by policy", 25);
            return HU_OK;
        }
    }

    int fds[2];
    if (pipe(fds) != 0) {
        *out = hu_tool_result_fail("pipe failed", 11);
        return HU_OK;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        *out = hu_tool_result_fail("fork failed", 11);
        return HU_OK;
    }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);

        if (s->workspace_dir && s->workspace_dir_len > 0) {
            char *wd = (char *)malloc(s->workspace_dir_len + 1);
            if (wd) {
                memcpy(wd, s->workspace_dir, s->workspace_dir_len);
                wd[s->workspace_dir_len] = '\0';
                if (chdir(wd) != 0)
                    _exit(127);
                free(wd);
            }
        }

        setenv("PATH", "/usr/bin:/bin", 1);

        /* Apply network proxy env vars if configured */
        if (s->policy && s->policy->net_proxy && s->policy->net_proxy->enabled) {
            const char *addr = s->policy->net_proxy->proxy_addr;
            if (!addr)
                addr = "http://127.0.0.1:0";
            setenv("HTTP_PROXY", addr, 1);
            setenv("HTTPS_PROXY", addr, 1);
            setenv("http_proxy", addr, 1);
            setenv("https_proxy", addr, 1);
            if (s->policy->net_proxy->allowed_domains_count > 0) {
                size_t total = 0;
                for (size_t i = 0; i < s->policy->net_proxy->allowed_domains_count; i++) {
                    if (s->policy->net_proxy->allowed_domains[i])
                        total += strlen(s->policy->net_proxy->allowed_domains[i]) + 1;
                }
                if (total > 0) {
                    char *no_proxy = (char *)malloc(total + 1);
                    if (no_proxy) {
                        size_t off = 0;
                        for (size_t i = 0; i < s->policy->net_proxy->allowed_domains_count; i++) {
                            const char *d = s->policy->net_proxy->allowed_domains[i];
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

        /* Apply kernel-level sandbox (Landlock, seccomp) */
        if (s->policy && s->policy->sandbox && s->policy->sandbox->vtable &&
            s->policy->sandbox->vtable->apply) {
            hu_error_t serr = s->policy->sandbox->vtable->apply(s->policy->sandbox->ctx);
            if (serr != HU_OK && serr != HU_ERR_NOT_SUPPORTED)
                _exit(125);
        }

        /* Wrap command with sandbox if available (argv-wrapping backends) */
        if (s->policy && s->policy->sandbox && hu_sandbox_is_available(s->policy->sandbox)) {
            const char *orig_argv[] = {"/bin/sh", "-c", cmd, NULL};
            const char *wrapped[16];
            size_t wrapped_count = 0;
            if (hu_sandbox_wrap_command(s->policy->sandbox, orig_argv, 3, wrapped, 15,
                                        &wrapped_count) == HU_OK &&
                wrapped_count > 0) {
                wrapped[wrapped_count] = NULL;
                execvp(wrapped[0], (char *const *)wrapped);
                _exit(127);
            }
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    size_t cap = 4096;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        close(fds[0]);
        waitpid(pid, NULL, 0);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_OK;
    }
    size_t len = 0;
    for (;;) {
        if (len >= cap - 1)
            break;
        ssize_t n = read(fds[0], buf + len, cap - len - 1);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    close(fds[0]);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        char *out_copy = hu_strndup(alloc, buf, len);
        alloc->free(alloc->ctx, buf, cap);
        if (!out_copy) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_OK;
        }
        *out = hu_tool_result_ok_owned(out_copy, len);
    } else if (WIFSIGNALED(status)) {
        alloc->free(alloc->ctx, buf, cap);
        char err[64];
        int n = snprintf(err, sizeof(err), "killed by signal %d", WTERMSIG(status));
        char *err_dup = hu_strndup(alloc, err, (size_t)n);
        if (err_dup)
            *out = hu_tool_result_fail_owned(err_dup, (size_t)n);
        else
            *out = hu_tool_result_fail("killed by signal", 16);
    } else {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        alloc->free(alloc->ctx, buf, cap);
        char err[64];
        int n = snprintf(err, sizeof(err), "exit code %d", code);
        char *err_dup = hu_strndup(alloc, err, (size_t)n);
        if (err_dup)
            *out = hu_tool_result_fail_owned(err_dup, (size_t)n);
        else
            *out = hu_tool_result_fail("command failed", 14);
    }
    return HU_OK;
#else
    (void)alloc;
    *out = hu_tool_result_fail("shell not supported on this platform", 38);
    return HU_OK;
#endif
#endif
}

static const char *shell_name(void *ctx) {
    (void)ctx;
    return HU_SHELL_NAME;
}

static const char *shell_description(void *ctx) {
    (void)ctx;
    return HU_SHELL_DESC;
}

static const char *shell_parameters_json(void *ctx) {
    (void)ctx;
    return HU_SHELL_PARAMS;
}

static void shell_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx)
        return;
    hu_shell_ctx_t *s = (hu_shell_ctx_t *)ctx;
    if (s->workspace_dir && alloc)
        alloc->free(alloc->ctx, (void *)s->workspace_dir, s->workspace_dir_len + 1);
    if (alloc)
        alloc->free(alloc->ctx, s, sizeof(*s));
}

static const hu_tool_vtable_t shell_vtable = {
    .execute = shell_execute,
    .name = shell_name,
    .description = shell_description,
    .parameters_json = shell_parameters_json,
    .deinit = shell_deinit,
};

hu_error_t hu_shell_create(hu_allocator_t *alloc, const char *workspace_dir,
                           size_t workspace_dir_len, hu_security_policy_t *policy, hu_tool_t *out) {
    hu_shell_ctx_t *s = (hu_shell_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*s));
    if (!s)
        return HU_ERR_OUT_OF_MEMORY;
    memset(s, 0, sizeof(*s));

    if (workspace_dir && workspace_dir_len > 0) {
        s->workspace_dir = hu_strndup(alloc, workspace_dir, workspace_dir_len);
        if (!s->workspace_dir) {
            alloc->free(alloc->ctx, s, sizeof(*s));
            return HU_ERR_OUT_OF_MEMORY;
        }
        s->workspace_dir_len = workspace_dir_len;
    }
    s->policy = policy;

    out->ctx = s;
    out->vtable = &shell_vtable;
    return HU_OK;
}
