#include "human/core/process_util.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"
#include "human/security/sandbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

bool hu_process_self_exe_path(char *buf, size_t cap) {
    if (!buf || cap < 2)
        return false;
    buf[0] = '\0';
#if defined(__APPLE__)
    char tmp[4096];
    uint32_t size = (uint32_t)sizeof(tmp);
    if (_NSGetExecutablePath(tmp, &size) != 0)
        return false;
    char *resolved = realpath(tmp, NULL);
    const char *src = resolved ? resolved : tmp;
    size_t n = strlen(src);
    bool ok = n > 0 && n < cap;
    if (ok)
        memcpy(buf, src, n + 1);
    free(resolved);
    return ok;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n <= 0) {
        buf[0] = '\0';
        return false;
    }
    buf[n] = '\0';
    return true;
#else
    return false;
#endif
}

#ifdef HU_GATEWAY_POSIX
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if (defined(__unix__) || defined(__APPLE__)) && !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

void hu_run_result_free(hu_allocator_t *alloc, hu_run_result_t *r) {
    if (!alloc || !r)
        return;
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

#if defined(HU_GATEWAY_POSIX) && !defined(HU_IS_TEST)
hu_error_t hu_process_run_sandboxed(hu_allocator_t *alloc, const char *const *argv, const char *cwd,
                                    size_t max_output_bytes, hu_child_setup_fn child_setup,
                                    void *child_setup_ctx, hu_run_result_t *out) {
    if (!alloc || !argv || !argv[0] || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (max_output_bytes == 0)
        max_output_bytes = 1048576;

    memset(out, 0, sizeof(*out));
    out->exit_code = -1;

    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0)
        return HU_ERR_IO;
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return HU_ERR_IO;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return HU_ERR_IO;
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
            hu_error_t serr = child_setup(child_setup_ctx);
            if (serr != HU_OK) {
                _exit(125);
            }
        }

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    size_t cap = max_output_bytes + 1;
    if (cap > 65536)
        cap = 65536;

    char *out_buf = (char *)alloc->alloc(alloc->ctx, cap);
    char *err_buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!out_buf || !err_buf) {
        if (out_buf)
            alloc->free(alloc->ctx, out_buf, cap);
        if (err_buf)
            alloc->free(alloc->ctx, err_buf, cap);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        waitpid(pid, NULL, 0);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t out_len = 0, err_len = 0;
    int stdout_eof = 0, stderr_eof = 0;
    fd_set rfds;
    int nfds = (stdout_pipe[0] > stderr_pipe[0]) ? stdout_pipe[0] + 1 : stderr_pipe[0] + 1;

    while (!stdout_eof || !stderr_eof) {
        FD_ZERO(&rfds);
        if (!stdout_eof && out_len < max_output_bytes)
            FD_SET(stdout_pipe[0], &rfds);
        if (!stderr_eof && err_len < max_output_bytes)
            FD_SET(stderr_pipe[0], &rfds);

        if (stdout_eof && stderr_eof)
            break;

        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int r = select(nfds, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0) {
            int status;
            if (waitpid(pid, &status, WNOHANG) == pid)
                break;
            continue;
        }

        if (!stdout_eof && FD_ISSET(stdout_pipe[0], &rfds)) {
            ssize_t n = read(stdout_pipe[0], out_buf + out_len, cap - out_len - 1);
            if (n > 0)
                out_len += (size_t)n;
            else
                stdout_eof = 1;
        }
        if (!stderr_eof && FD_ISSET(stderr_pipe[0], &rfds)) {
            ssize_t n = read(stderr_pipe[0], err_buf + err_len, cap - err_len - 1);
            if (n > 0)
                err_len += (size_t)n;
            else
                stderr_eof = 1;
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

    return HU_OK;
}

hu_error_t hu_process_run(hu_allocator_t *alloc, const char *const *argv, const char *cwd,
                          size_t max_output_bytes, hu_run_result_t *out) {
    return hu_process_run_sandboxed(alloc, argv, cwd, max_output_bytes, NULL, NULL, out);
}

hu_error_t hu_process_run_with_timeout(hu_allocator_t *alloc, const char *const *argv,
                                       const char *cwd, size_t max_output_bytes,
                                       unsigned int timeout_sec, hu_run_result_t *out) {
    if (timeout_sec == 0)
        return hu_process_run(alloc, argv, cwd, max_output_bytes, out);

    /* Build wrapper: perl -e 'alarm N; exec @ARGV' <original argv...> */
    size_t argc = 0;
    while (argv[argc])
        argc++;
    if (argc + 4 > 64)
        return hu_process_run(alloc, argv, cwd, max_output_bytes, out);

    const char *wrapped[68];
    char alarm_expr[48];
    snprintf(alarm_expr, sizeof(alarm_expr), "alarm %u; exec @ARGV", timeout_sec);
    wrapped[0] = "perl";
    wrapped[1] = "-e";
    wrapped[2] = alarm_expr;
    for (size_t i = 0; i <= argc; i++)
        wrapped[3 + i] = argv[i];
    return hu_process_run(alloc, wrapped, cwd, max_output_bytes, out);
}

typedef struct hu_policy_child_ctx {
    hu_security_policy_t *policy;
    const char *const *argv;
    size_t argc;
} hu_policy_child_ctx_t;

static hu_error_t policy_child_setup(void *raw) {
    hu_policy_child_ctx_t *pc = (hu_policy_child_ctx_t *)raw;
    if (!pc || !pc->policy)
        return HU_OK;
    hu_security_policy_t *p = pc->policy;

    if (p->net_proxy && p->net_proxy->enabled) {
        const char *addr = p->net_proxy->proxy_addr;
        if (!addr)
            addr = "http://127.0.0.1:0";
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
                /* no allocator in scope — raw malloc */
                char *no_proxy = (char *)malloc(total + 1);
                if (no_proxy) {
                    size_t off = 0;
                    for (size_t i = 0; i < p->net_proxy->allowed_domains_count; i++) {
                        const char *d = p->net_proxy->allowed_domains[i];
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

    if (p->sandbox && p->sandbox->vtable && p->sandbox->vtable->apply) {
        hu_error_t err = p->sandbox->vtable->apply(p->sandbox->ctx);
        if (err != HU_OK && err != HU_ERR_NOT_SUPPORTED)
            return err;
    }

    if (p->sandbox && hu_sandbox_is_available(p->sandbox) && pc->argv && pc->argc > 0) {
        const char *wrapped[16];
        size_t wrapped_count = 0;
        if (hu_sandbox_wrap_command(p->sandbox, pc->argv, pc->argc, wrapped, 15, &wrapped_count) ==
                HU_OK &&
            wrapped_count > 0) {
            wrapped[wrapped_count] = NULL;
            execvp(wrapped[0], (char *const *)wrapped);
            _exit(127);
        }
    }

    return HU_OK;
}

hu_error_t hu_process_run_with_policy(hu_allocator_t *alloc, const char *const *argv,
                                      const char *cwd, size_t max_output_bytes,
                                      hu_security_policy_t *policy, hu_run_result_t *out) {
    if (!alloc || !argv || !argv[0] || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!policy) {
        return hu_process_run(alloc, argv, cwd, max_output_bytes, out);
    }
    size_t argc = 0;
    while (argv[argc])
        argc++;
    hu_policy_child_ctx_t ctx = {.policy = policy, .argv = argv, .argc = argc};
    return hu_process_run_sandboxed(alloc, argv, cwd, max_output_bytes, policy_child_setup, &ctx,
                                    out);
}

#else
/* Non-POSIX or HU_IS_TEST: stub that returns empty success */
hu_error_t hu_process_run_sandboxed(hu_allocator_t *alloc, const char *const *argv, const char *cwd,
                                    size_t max_output_bytes, hu_child_setup_fn child_setup,
                                    void *child_setup_ctx, hu_run_result_t *out) {
    (void)cwd;
    (void)max_output_bytes;
    (void)child_setup;
    (void)child_setup_ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!argv || !argv[0])
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->stdout_buf = (char *)alloc->alloc(alloc->ctx, 1);
    if (!out->stdout_buf)
        return HU_ERR_OUT_OF_MEMORY;
    out->stdout_buf[0] = '\0';
    out->stdout_cap = 1;
    out->stderr_buf = (char *)alloc->alloc(alloc->ctx, 1);
    if (!out->stderr_buf) {
        alloc->free(alloc->ctx, out->stdout_buf, 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->stderr_buf[0] = '\0';
    out->stderr_cap = 1;
    out->success = true;
    out->exit_code = 0;
    return HU_OK;
}

hu_error_t hu_process_run(hu_allocator_t *alloc, const char *const *argv, const char *cwd,
                          size_t max_output_bytes, hu_run_result_t *out) {
    return hu_process_run_sandboxed(alloc, argv, cwd, max_output_bytes, NULL, NULL, out);
}

hu_error_t hu_process_run_with_timeout(hu_allocator_t *alloc, const char *const *argv,
                                       const char *cwd, size_t max_output_bytes,
                                       unsigned int timeout_sec, hu_run_result_t *out) {
    (void)timeout_sec;
    return hu_process_run(alloc, argv, cwd, max_output_bytes, out);
}

hu_error_t hu_process_run_with_policy(hu_allocator_t *alloc, const char *const *argv,
                                      const char *cwd, size_t max_output_bytes,
                                      hu_security_policy_t *policy, hu_run_result_t *out) {
    (void)policy;
    if (!alloc || !argv || !argv[0] || !out)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_process_run(alloc, argv, cwd, max_output_bytes, out);
}
#endif

bool hu_exe_on_path(const char *name) {
#if defined(_WIN32)
    (void)name;
    return false;
#elif defined(__unix__) || defined(__APPLE__)
    if (!name || !name[0])
        return false;
    if (strchr(name, '/'))
        return access(name, X_OK) == 0;
    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0])
        return false;
    char dir[4096];
    char cand[4096];
    const char *p = path_env;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t plen = colon ? (size_t)(colon - p) : strlen(p);
        if (plen >= sizeof(dir)) {
            if (!colon)
                break;
            p = colon + 1;
            continue;
        }
        memcpy(dir, p, plen);
        dir[plen] = '\0';
        while (plen > 1 && dir[plen - 1] == '/')
            dir[--plen] = '\0';
        const char *d = plen ? dir : ".";
        int n = snprintf(cand, sizeof cand, "%s/%s", d, name);
        if (n > 0 && (size_t)n < sizeof(cand) && access(cand, X_OK) == 0)
            return true;
        if (!colon)
            break;
        p = colon + 1;
    }
    return false;
#else
    (void)name;
    return false;
#endif
}

bool hu_ollama_api_tags_reachable(void) {
#if defined(_WIN32) || !(defined(__unix__) || defined(__APPLE__))
    return false;
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, (socklen_t)sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(11434);
    addr.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
        close(fd);
        return false;
    }
    static const char req[] =
        "GET /api/tags HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if (send(fd, req, sizeof(req) - 1, 0) < 0) {
        close(fd);
        return false;
    }
    char buf[256];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    return strstr(buf, " 200 ") != NULL || strstr(buf, "200 OK") != NULL;
#endif
}

bool hu_mlx_lm_module_available(void) {
#if HU_IS_TEST
    return false;
#elif defined(_WIN32) || !defined(HU_GATEWAY_POSIX)
    return false;
#else
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"python3", "-c", "import mlx_lm", NULL};
    hu_run_result_t rr = {0};
    hu_error_t err = hu_process_run(&alloc, argv, NULL, 65536, &rr);
    if (err != HU_OK) {
        hu_run_result_free(&alloc, &rr);
        return false;
    }
    bool ok = rr.success && rr.exit_code == 0;
    hu_run_result_free(&alloc, &rr);
    return ok;
#endif
}
