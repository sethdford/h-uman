#include "seaclaw/core/process_util.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/security.h"
#include "seaclaw/security/sandbox.h"
#include <string.h>
#include <stdlib.h>

#ifdef SC_GATEWAY_POSIX
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#endif

void sc_run_result_free(sc_allocator_t *alloc, sc_run_result_t *r) {
    if (!alloc || !r) return;
    if (r->stdout_buf) {
        alloc->free(alloc->ctx, r->stdout_buf, r->stdout_cap);
        r->stdout_buf = NULL;
    }
    if (r->stderr_buf) {
        alloc->free(alloc->ctx, r->stderr_buf, r->stderr_cap);
        r->stderr_buf = NULL;
    }
    r->stdout_len = 0;
    r->stdout_cap = 0;
    r->stderr_len = 0;
    r->stderr_cap = 0;
}

#if defined(SC_GATEWAY_POSIX) && !defined(SC_IS_TEST)
sc_error_t sc_process_run_sandboxed(sc_allocator_t *alloc,
    const char *const *argv,
    const char *cwd,
    size_t max_output_bytes,
    sc_child_setup_fn child_setup,
    void *child_setup_ctx,
    sc_run_result_t *out)
{
    if (!alloc || !argv || !argv[0] || !out) return SC_ERR_INVALID_ARGUMENT;
    if (max_output_bytes == 0) max_output_bytes = 1048576;

    memset(out, 0, sizeof(*out));
    out->exit_code = -1;

    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0)
        return SC_ERR_IO;

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return SC_ERR_IO;
    }

    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        if (cwd && cwd[0]) {
            if (chdir(cwd) != 0) {
                _exit(126);
            }
        }

        /* Apply sandbox restrictions before running the command */
        if (child_setup) {
            sc_error_t serr = child_setup(child_setup_ctx);
            if (serr != SC_OK) {
                _exit(125);
            }
        }

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    size_t cap = max_output_bytes + 1;
    if (cap > 65536) cap = 65536;

    char *out_buf = (char *)alloc->alloc(alloc->ctx, cap);
    char *err_buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!out_buf || !err_buf) {
        if (out_buf) alloc->free(alloc->ctx, out_buf, cap);
        if (err_buf) alloc->free(alloc->ctx, err_buf, cap);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        waitpid(pid, NULL, 0);
        return SC_ERR_OUT_OF_MEMORY;
    }

    size_t out_len = 0, err_len = 0;
    int stdout_eof = 0, stderr_eof = 0;
    fd_set rfds;
    int nfds = (stdout_pipe[0] > stderr_pipe[0]) ? stdout_pipe[0] + 1 : stderr_pipe[0] + 1;

    while (!stdout_eof || !stderr_eof) {
        FD_ZERO(&rfds);
        if (!stdout_eof && out_len < max_output_bytes) FD_SET(stdout_pipe[0], &rfds);
        if (!stderr_eof && err_len < max_output_bytes) FD_SET(stderr_pipe[0], &rfds);

        if (stdout_eof && stderr_eof) break;

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(nfds, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) {
            int status;
            if (waitpid(pid, &status, WNOHANG) == pid) break;
            continue;
        }

        if (!stdout_eof && FD_ISSET(stdout_pipe[0], &rfds)) {
            ssize_t n = read(stdout_pipe[0], out_buf + out_len, cap - out_len - 1);
            if (n > 0) out_len += (size_t)n;
            else stdout_eof = 1;
        }
        if (!stderr_eof && FD_ISSET(stderr_pipe[0], &rfds)) {
            ssize_t n = read(stderr_pipe[0], err_buf + err_len, cap - err_len - 1);
            if (n > 0) err_len += (size_t)n;
            else stderr_eof = 1;
        }
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status;
    waitpid(pid, &status, 0);

    out_buf[out_len] = '\0';
    err_buf[err_len] = '\0';
    out->stdout_buf = out_buf;
    out->stdout_len = out_len;
    out->stdout_cap = cap;
    out->stderr_buf = err_buf;
    out->stderr_len = err_len;
    out->stderr_cap = cap;

    if (WIFEXITED(status)) {
        out->exit_code = WEXITSTATUS(status);
        out->success = (out->exit_code == 0);
    } else {
        out->exit_code = -1;
        out->success = false;
    }

    return SC_OK;
}

sc_error_t sc_process_run(sc_allocator_t *alloc,
    const char *const *argv,
    const char *cwd,
    size_t max_output_bytes,
    sc_run_result_t *out)
{
    return sc_process_run_sandboxed(alloc, argv, cwd, max_output_bytes,
        NULL, NULL, out);
}

typedef struct sc_policy_child_ctx {
    sc_security_policy_t *policy;
} sc_policy_child_ctx_t;

static sc_error_t policy_child_setup(void *raw) {
    sc_policy_child_ctx_t *pc = (sc_policy_child_ctx_t *)raw;
    if (!pc || !pc->policy) return SC_OK;
    sc_security_policy_t *p = pc->policy;

    if (p->net_proxy && p->net_proxy->enabled) {
        const char *addr = p->net_proxy->proxy_addr;
        if (!addr) addr = "http://127.0.0.1:0";
        setenv("HTTP_PROXY", addr, 1);
        setenv("HTTPS_PROXY", addr, 1);
        setenv("http_proxy", addr, 1);
        setenv("https_proxy", addr, 1);
        if (p->net_proxy->allowed_domains_count > 0) {
            size_t total = 0;
            for (size_t i = 0; i < p->net_proxy->allowed_domains_count; i++) {
                if (p->net_proxy->allowed_domains[i])
                    total += strlen(p->net_proxy->allowed_domains[i]) + 1;
            }
            if (total > 0) {
                char *no_proxy = (char *)malloc(total + 1);
                if (no_proxy) {
                    size_t off = 0;
                    for (size_t i = 0; i < p->net_proxy->allowed_domains_count; i++) {
                        const char *d = p->net_proxy->allowed_domains[i];
                        if (!d) continue;
                        size_t dlen = strlen(d);
                        if (off > 0) no_proxy[off++] = ',';
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

    if (p->sandbox && p->sandbox->vtable && p->sandbox->vtable->apply) {
        sc_error_t err = p->sandbox->vtable->apply(p->sandbox->ctx);
        if (err != SC_OK && err != SC_ERR_NOT_SUPPORTED)
            return err;
    }

    return SC_OK;
}

sc_error_t sc_process_run_with_policy(sc_allocator_t *alloc,
    const char *const *argv,
    const char *cwd,
    size_t max_output_bytes,
    sc_security_policy_t *policy,
    sc_run_result_t *out)
{
    if (!policy) {
        return sc_process_run(alloc, argv, cwd, max_output_bytes, out);
    }
    sc_policy_child_ctx_t ctx = { .policy = policy };
    return sc_process_run_sandboxed(alloc, argv, cwd, max_output_bytes,
        policy_child_setup, &ctx, out);
}

#else
/* Non-POSIX or SC_IS_TEST: stub that returns empty success */
sc_error_t sc_process_run_sandboxed(sc_allocator_t *alloc,
    const char *const *argv,
    const char *cwd,
    size_t max_output_bytes,
    sc_child_setup_fn child_setup,
    void *child_setup_ctx,
    sc_run_result_t *out)
{
    (void)cwd;
    (void)max_output_bytes;
    (void)child_setup;
    (void)child_setup_ctx;
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    if (!argv || !argv[0]) return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->stdout_buf = (char *)alloc->alloc(alloc->ctx, 1);
    if (!out->stdout_buf) return SC_ERR_OUT_OF_MEMORY;
    out->stdout_buf[0] = '\0';
    out->stdout_cap = 1;
    out->stderr_buf = (char *)alloc->alloc(alloc->ctx, 1);
    if (!out->stderr_buf) {
        alloc->free(alloc->ctx, out->stdout_buf, 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    out->stderr_buf[0] = '\0';
    out->stderr_cap = 1;
    out->success = true;
    out->exit_code = 0;
    return SC_OK;
}

sc_error_t sc_process_run(sc_allocator_t *alloc,
    const char *const *argv,
    const char *cwd,
    size_t max_output_bytes,
    sc_run_result_t *out)
{
    return sc_process_run_sandboxed(alloc, argv, cwd, max_output_bytes,
        NULL, NULL, out);
}

sc_error_t sc_process_run_with_policy(sc_allocator_t *alloc,
    const char *const *argv,
    const char *cwd,
    size_t max_output_bytes,
    sc_security_policy_t *policy,
    sc_run_result_t *out)
{
    (void)policy;
    return sc_process_run(alloc, argv, cwd, max_output_bytes, out);
}
#endif
