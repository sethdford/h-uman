/*
 * Git operations tool — status, diff, log, branch, commit, add, checkout, stash.
 */
#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/security.h"
#include "seaclaw/security/sandbox.h"
#include "seaclaw/tools/validation.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#endif

#define SC_GIT_NAME "git_operations"
#define SC_GIT_DESC "Perform Git operations (status, diff, log, branch, commit, add, checkout, stash)."
#define SC_GIT_PARAMS "{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\",\"enum\":[\"status\",\"diff\",\"log\",\"branch\",\"commit\",\"add\",\"checkout\",\"stash\"]},\"message\":{\"type\":\"string\"},\"paths\":{\"type\":\"string\"},\"branch\":{\"type\":\"string\"},\"files\":{\"type\":\"string\"},\"cached\":{\"type\":\"boolean\"},\"limit\":{\"type\":\"integer\"},\"action\":{\"type\":\"string\"}},\"required\":[\"operation\"]}"
#define SC_GIT_OUTPUT_MAX 1048576

typedef struct sc_git_ctx {
    const char *workspace_dir;
    size_t workspace_dir_len;
    sc_security_policy_t *policy;
} sc_git_ctx_t;

static bool sanitize_git_args(const char *args)
{
    if (!args) return true;
    if (strstr(args, "$(") || strchr(args, '`') || strchr(args, '|') || strchr(args, ';'))
        return false;
    if (strstr(args, "--exec=") || strstr(args, "--no-verify")) return false;
    return true;
}

#if !SC_IS_TEST
static char *run_git(sc_allocator_t *alloc, const char *cwd, const char **argv, int argc,
    sc_security_policy_t *policy, sc_tool_result_t *out)
{
#ifndef _WIN32
    int fds[2];
    if (pipe(fds) != 0) { *out = sc_tool_result_fail("pipe failed", 11); return NULL; }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        *out = sc_tool_result_fail("fork failed", 11);
        return NULL;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        if (cwd && cwd[0]) chdir(cwd);
        setenv("PATH", "/usr/bin:/bin", 1);

        if (policy && policy->net_proxy && policy->net_proxy->enabled) {
            const char *addr = policy->net_proxy->proxy_addr;
            if (!addr) addr = "http://127.0.0.1:0";
            setenv("HTTP_PROXY", addr, 1);
            setenv("HTTPS_PROXY", addr, 1);
            setenv("http_proxy", addr, 1);
            setenv("https_proxy", addr, 1);
            if (policy->net_proxy->allowed_domains_count > 0) {
                char no_proxy[4096];
                size_t off = 0;
                for (size_t i = 0; i < policy->net_proxy->allowed_domains_count; i++) {
                    const char *d = policy->net_proxy->allowed_domains[i];
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

        if (policy && policy->sandbox && policy->sandbox->vtable &&
            policy->sandbox->vtable->apply) {
            sc_error_t serr = policy->sandbox->vtable->apply(
                policy->sandbox->ctx);
            if (serr != SC_OK && serr != SC_ERR_NOT_SUPPORTED)
                _exit(125);
        }

        /* Wrap command with sandbox if available (firejail, seatbelt, etc.) */
        if (policy && policy->sandbox &&
            sc_sandbox_is_available(policy->sandbox)) {
            const char *wrapped[32];
            size_t wrapped_count = 0;
            if (sc_sandbox_wrap_command(policy->sandbox,
                    argv, (size_t)argc, wrapped, 31, &wrapped_count) == SC_OK &&
                    wrapped_count > 0) {
                wrapped[wrapped_count] = NULL;
                execvp(wrapped[0], (char *const *)wrapped);
                _exit(127);
            }
        }

        char **exec_argv = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
        if (!exec_argv) _exit(127);
        for (int i = 0; i < argc; i++) exec_argv[i] = (char *)argv[i];
        exec_argv[argc] = NULL;
        execv("/usr/bin/git", exec_argv);
        execv("/bin/git", exec_argv);
        free(exec_argv);
        _exit(127);
    }
    close(fds[1]);
    size_t cap = 65536;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) { close(fds[0]); waitpid(pid, NULL, 0); *out = sc_tool_result_fail("out of memory", 12); return NULL; }
    size_t len = 0;
    for (;;) {
        if (len >= cap - 1 || len >= SC_GIT_OUTPUT_MAX) break;
        ssize_t n = read(fds[0], buf + len, cap - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
        if (len >= cap - 1 && cap < SC_GIT_OUTPUT_MAX) {
            size_t new_cap = cap * 2;
            if (new_cap > SC_GIT_OUTPUT_MAX) new_cap = SC_GIT_OUTPUT_MAX;
            char *nbuf = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nbuf) break;
            buf = nbuf;
            cap = new_cap;
        }
    }
    buf[len] = '\0';
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        char err_buf[48];
        int n = snprintf(err_buf, sizeof(err_buf), "Git failed: exit %d", WEXITSTATUS(status));
        char *err_msg = sc_strndup(alloc, err_buf, (size_t)n);
        alloc->free(alloc->ctx, buf, cap);
        *out = err_msg ? sc_tool_result_fail_owned(err_msg, (size_t)n) : sc_tool_result_fail("Git failed", 10);
        return NULL;
    }
    if (cap > len + 1) {
        char *t = (char *)alloc->realloc(alloc->ctx, buf, cap, len + 1);
        if (t) buf = t;
    }
    return buf;
#else
    (void)alloc;(void)cwd;(void)argv;(void)argc;
    *out = sc_tool_result_fail("git not supported on this platform", 35);
    return NULL;
#endif
}
#endif

static sc_error_t git_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_git_ctx_t *c = (sc_git_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *op = sc_json_get_string(args, "operation");
    if (!op || strlen(op) == 0) {
        *out = sc_tool_result_fail("Missing 'operation' parameter", 27);
        return SC_OK;
    }
    const char *fields[] = { "message", "paths", "branch", "files", "action" };
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
        const char *val = sc_json_get_string(args, fields[i]);
        if (val && !sanitize_git_args(val)) {
            *out = sc_tool_result_fail("Unsafe git arguments detected", 28);
            return SC_OK;
        }
    }
#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(git stub in test)", 18);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(msg, 18);
    return SC_OK;
#else
#ifndef _WIN32
    const char *cwd = c->workspace_dir && c->workspace_dir_len > 0 ? c->workspace_dir : ".";
    char limit_buf[24];
    const char *argv[24];
    int argc = 0;
    argv[argc++] = "git";
    if (strcmp(op, "status") == 0) {
        argv[argc++] = "status"; argv[argc++] = "--porcelain=2"; argv[argc++] = "--branch";
    } else if (strcmp(op, "diff") == 0) {
        argv[argc++] = "diff"; argv[argc++] = "--unified=3";
        if (sc_json_get_bool(args, "cached", false)) argv[argc++] = "--cached";
        {
            const char *files = sc_json_get_string(args, "files");
            const char *files_arg = files && files[0] ? files : ".";
            if (strcmp(files_arg, ".") != 0) {
                sc_error_t err = sc_tool_validate_path(files_arg,
                    c->workspace_dir, c->workspace_dir ? c->workspace_dir_len : 0);
                if (err != SC_OK) {
                    *out = sc_tool_result_fail("path traversal or invalid path", 30);
                    return SC_OK;
                }
            }
            argv[argc++] = "--"; argv[argc++] = files_arg;
        }
    } else if (strcmp(op, "log") == 0) {
        int lim = (int)sc_json_get_number(args, "limit", 10);
        if (lim < 1) lim = 1; if (lim > 1000) lim = 1000;
        snprintf(limit_buf, sizeof(limit_buf), "-%d", lim);
        argv[argc++] = "log"; argv[argc++] = limit_buf;
        argv[argc++] = "--pretty=format:%H|%an|%ae|%ad|%s"; argv[argc++] = "--date=iso";
    } else if (strcmp(op, "branch") == 0) {
        argv[argc++] = "branch"; argv[argc++] = "--format=%(refname:short)|%(HEAD)";
    } else if (strcmp(op, "commit") == 0) {
        const char *m = sc_json_get_string(args, "message");
        if (!m || !m[0]) { *out = sc_tool_result_fail("Missing 'message' for commit", 27); return SC_OK; }
        argv[argc++] = "commit"; argv[argc++] = "-m"; argv[argc++] = m;
    } else if (strcmp(op, "add") == 0) {
        const char *p = sc_json_get_string(args, "paths");
        if (!p || !p[0]) { *out = sc_tool_result_fail("Missing 'paths' for add", 22); return SC_OK; }
        argv[argc++] = "add"; argv[argc++] = "--"; argv[argc++] = p;
    } else if (strcmp(op, "checkout") == 0) {
        const char *b = sc_json_get_string(args, "branch");
        if (!b || !b[0]) { *out = sc_tool_result_fail("Missing 'branch' for checkout", 30); return SC_OK; }
        if (strchr(b,';')||strchr(b,'|')||strchr(b,'`')||strstr(b,"$(")) {
            *out = sc_tool_result_fail("Branch name contains invalid characters", 36);
            return SC_OK;
        }
        if (strstr(b, "..")) {
            *out = sc_tool_result_fail("invalid branch name", 19);
            return SC_OK;
        }
        argv[argc++] = "checkout"; argv[argc++] = b;
    } else if (strcmp(op, "stash") == 0) {
        const char *a = sc_json_get_string(args, "action");
        if (!a) a = "push";
        if (strcmp(a,"push")==0 || strcmp(a,"save")==0) {
            argv[argc++] = "stash"; argv[argc++] = "push"; argv[argc++] = "-m"; argv[argc++] = "auto-stash";
        } else if (strcmp(a,"pop")==0) { argv[argc++] = "stash"; argv[argc++] = "pop"; }
        else if (strcmp(a,"list")==0) { argv[argc++] = "stash"; argv[argc++] = "list"; }
        else { *out = sc_tool_result_fail("Unknown stash action", 19); return SC_OK; }
    } else {
        *out = sc_tool_result_fail("Unknown operation", 17);
        return SC_OK;
    }
    char *result = run_git(alloc, cwd, argv, argc, c->policy, out);
    if (!result) return SC_OK;
    *out = sc_tool_result_ok_owned(result, strlen(result));
    return SC_OK;
#else
    *out = sc_tool_result_fail("git not supported on this platform", 35);
    return SC_OK;
#endif
#endif
}

static const char *git_name(void *ctx) { (void)ctx; return SC_GIT_NAME; }
static const char *git_description(void *ctx) { (void)ctx; return SC_GIT_DESC; }
static const char *git_parameters_json(void *ctx) { (void)ctx; return SC_GIT_PARAMS; }
static void git_deinit(void *ctx, sc_allocator_t *alloc) {
    if (!ctx) return;
    sc_git_ctx_t *c = (sc_git_ctx_t *)ctx;
    if (c->workspace_dir && alloc)
        alloc->free(alloc->ctx, (void *)c->workspace_dir, c->workspace_dir_len + 1);
    free(c);
}

static const sc_tool_vtable_t git_vtable = {
    .execute = git_execute, .name = git_name,
    .description = git_description, .parameters_json = git_parameters_json,
    .deinit = git_deinit,
};

sc_error_t sc_git_create(sc_allocator_t *alloc,
    const char *workspace_dir, size_t workspace_dir_len,
    sc_security_policy_t *policy,
    sc_tool_t *out)
{
    sc_git_ctx_t *c = (sc_git_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    if (workspace_dir && workspace_dir_len > 0) {
        c->workspace_dir = sc_strndup(alloc, workspace_dir, workspace_dir_len);
        if (!c->workspace_dir) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        c->workspace_dir_len = workspace_dir_len;
    }
    c->policy = policy;
    out->ctx = c;
    out->vtable = &git_vtable;
    return SC_OK;
}
