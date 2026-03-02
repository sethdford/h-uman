#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/security.h"
#include "seaclaw/security/sandbox.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#endif

#define SC_SHELL_NAME "shell"
#define SC_SHELL_DESC "Execute shell commands. Use with caution."
#define SC_SHELL_PARAMS "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}"
#define SC_SHELL_CMD_MAX 4096

typedef struct sc_shell_ctx {
    const char *workspace_dir;
    size_t workspace_dir_len;
    sc_security_policy_t *policy;
} sc_shell_ctx_t;

/*
 * SECURITY WARNING: This tool passes commands directly to /bin/sh -c.
 * Shell metacharacters in cmd are interpreted by the shell.
 * This tool should be restricted or disabled in high-assurance deployments.
 * Use sc_policy_validate_command and sandbox wrapping for mitigation.
 */
static sc_error_t shell_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_shell_ctx_t *s = (sc_shell_ctx_t *)ctx;
    if (!s || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 13);
        return SC_ERR_INVALID_ARGUMENT;
    }

#if SC_IS_TEST
    {
        const char *stub = "(shell disabled in test mode)";
        size_t stub_len = strlen(stub);
        char *msg = sc_strndup(alloc, stub, stub_len);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 13); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, stub_len);
    }
    return SC_OK;
#else
#ifndef _WIN32
    if (s->policy && !sc_security_shell_allowed(s->policy)) {
        *out = sc_tool_result_fail("shell execution not allowed by policy", 38);
        return SC_OK;
    }

    const char *cmd = sc_json_get_string(args, "command");
    if (!cmd || strlen(cmd) == 0) {
        *out = sc_tool_result_fail("missing command", 15);
        return SC_OK;
    }
    size_t cmd_len = strlen(cmd);
    if (cmd_len > SC_SHELL_CMD_MAX) {
        *out = sc_tool_result_fail("command too long", 16);
        return SC_OK;
    }

    if (s->policy) {
        bool approved = s->policy->pre_approved;
        s->policy->pre_approved = false;
        sc_command_risk_level_t risk;
        sc_error_t perr = sc_policy_validate_command(s->policy, cmd, approved, &risk);
        if (perr == SC_ERR_SECURITY_APPROVAL_REQUIRED) {
            *out = sc_tool_result_fail("approval required", 17);
            out->needs_approval = true;
            return SC_OK;
        }
        if (perr != SC_OK) {
            *out = sc_tool_result_fail("command blocked by policy", 25);
            return SC_OK;
        }
    }

    int fds[2];
    if (pipe(fds) != 0) {
        *out = sc_tool_result_fail("pipe failed", 11);
        return SC_OK;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        *out = sc_tool_result_fail("fork failed", 11);
        return SC_OK;
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
                chdir(wd);
                free(wd);
            }
        }

        setenv("PATH", "/usr/bin:/bin", 1);

        /* Apply network proxy env vars if configured */
        if (s->policy && s->policy->net_proxy &&
            s->policy->net_proxy->enabled) {
            const char *addr = s->policy->net_proxy->proxy_addr;
            if (!addr) addr = "http://127.0.0.1:0";
            setenv("HTTP_PROXY", addr, 1);
            setenv("HTTPS_PROXY", addr, 1);
            setenv("http_proxy", addr, 1);
            setenv("https_proxy", addr, 1);
            if (s->policy->net_proxy->allowed_domains_count > 0) {
                char no_proxy[4096];
                size_t off = 0;
                for (size_t i = 0; i < s->policy->net_proxy->allowed_domains_count; i++) {
                    const char *d = s->policy->net_proxy->allowed_domains[i];
                    if (!d) continue;
                    size_t dlen = strlen(d);
                    if (off + dlen + 2 >= sizeof(no_proxy)) break;
                    if (off > 0) no_proxy[off++] = ',';
                    memcpy(no_proxy + off, d, dlen);
                    off += dlen;
                }
                no_proxy[off] = '\0';
                setenv("NO_PROXY", no_proxy, 1);
                setenv("no_proxy", no_proxy, 1);
            }
        }

        /* Apply kernel-level sandbox (Landlock, seccomp) */
        if (s->policy && s->policy->sandbox &&
            s->policy->sandbox->vtable &&
            s->policy->sandbox->vtable->apply) {
            sc_error_t serr = s->policy->sandbox->vtable->apply(
                s->policy->sandbox->ctx);
            if (serr != SC_OK && serr != SC_ERR_NOT_SUPPORTED)
                _exit(125);
        }

        /* Wrap command with sandbox if available (argv-wrapping backends) */
        if (s->policy && s->policy->sandbox &&
            sc_sandbox_is_available(s->policy->sandbox)) {
            const char *orig_argv[] = { "/bin/sh", "-c", cmd, NULL };
            const char *wrapped[16];
            size_t wrapped_count = 0;
            if (sc_sandbox_wrap_command(s->policy->sandbox,
                    orig_argv, 3, wrapped, 15, &wrapped_count) == SC_OK &&
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
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    for (;;) {
        if (len >= cap - 1) break;
        ssize_t n = read(fds[0], buf + len, cap - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    close(fds[0]);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        char *out_copy = sc_strndup(alloc, buf, len);
        alloc->free(alloc->ctx, buf, cap);
        if (!out_copy) {
            *out = sc_tool_result_fail("out of memory", 12);
            return SC_ERR_OUT_OF_MEMORY;
        }
        *out = sc_tool_result_ok_owned(out_copy, len);
    } else {
        char err[64];
        int n = snprintf(err, sizeof(err), "exit code %d", WEXITSTATUS(status));
        alloc->free(alloc->ctx, buf, cap);
        *out = sc_tool_result_fail(err, (size_t)n);
    }
    return SC_OK;
#else
    (void)alloc;
    *out = sc_tool_result_fail("shell not supported on this platform", 38);
    return SC_OK;
#endif
#endif
}

static const char *shell_name(void *ctx) {
    (void)ctx;
    return SC_SHELL_NAME;
}

static const char *shell_description(void *ctx) {
    (void)ctx;
    return SC_SHELL_DESC;
}

static const char *shell_parameters_json(void *ctx) {
    (void)ctx;
    return SC_SHELL_PARAMS;
}

static void shell_deinit(void *ctx, sc_allocator_t *alloc) {
    if (!ctx) return;
    sc_shell_ctx_t *s = (sc_shell_ctx_t *)ctx;
    if (s->workspace_dir && alloc)
        alloc->free(alloc->ctx, (void *)s->workspace_dir, s->workspace_dir_len + 1);
    free(s);
}

static const sc_tool_vtable_t shell_vtable = {
    .execute = shell_execute,
    .name = shell_name,
    .description = shell_description,
    .parameters_json = shell_parameters_json,
    .deinit = shell_deinit,
};

sc_error_t sc_shell_create(sc_allocator_t *alloc,
    const char *workspace_dir, size_t workspace_dir_len,
    sc_security_policy_t *policy,
    sc_tool_t *out)
{
    sc_shell_ctx_t *s = (sc_shell_ctx_t *)calloc(1, sizeof(*s));
    if (!s) return SC_ERR_OUT_OF_MEMORY;

    if (workspace_dir && workspace_dir_len > 0) {
        s->workspace_dir = sc_strndup(alloc, workspace_dir, workspace_dir_len);
        if (!s->workspace_dir) { free(s); return SC_ERR_OUT_OF_MEMORY; }
        s->workspace_dir_len = workspace_dir_len;
    }
    s->policy = policy;

    out->ctx = s;
    out->vtable = &shell_vtable;
    return SC_OK;
}
