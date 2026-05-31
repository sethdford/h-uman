#include "human/core/string.h"
#include "human/mcp_transport.h"
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#ifdef HU_GATEWAY_POSIX
#include <signal.h>
#include <sys/wait.h>
#endif

typedef struct stdio_ctx {
    int read_fd;
    int write_fd;
#ifdef HU_GATEWAY_POSIX
    int child_pid; /* -1 if FD-based (no child), >=0 if spawned */
    char *command; /* NULL if FD-based, owned if spawn */
    char **argv;   /* NULL if FD-based, owned if spawn */
    size_t argv_count;
#endif
} stdio_ctx_t;

static hu_error_t stdio_send(void *ctx, const char *data, size_t len) {
    stdio_ctx_t *c = (stdio_ctx_t *)ctx;
    if (!c || !data)
        return HU_ERR_INVALID_ARGUMENT;
#if defined(__unix__) || defined(__APPLE__)
    if (c->write_fd < 0)
        return HU_ERR_NOT_SUPPORTED;
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(c->write_fd, data + total, len - total);
        if (n < 0)
            return HU_ERR_IO;
        total += (size_t)n;
    }
    if (write(c->write_fd, "\n", 1) != 1)
        return HU_ERR_IO;
    return HU_OK;
#else
    (void)len;
    (void)c;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static hu_error_t stdio_recv(void *ctx, hu_allocator_t *alloc, char **out, size_t *out_len) {
    stdio_ctx_t *c = (stdio_ctx_t *)ctx;
    if (!c || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
#if defined(__unix__) || defined(__APPLE__)
    if (c->read_fd < 0)
        return HU_ERR_NOT_SUPPORTED;
    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t len = 0;
    for (;;) {
        if (len >= cap - 1) {
            size_t new_cap = cap * 2;
            char *nbuf = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nbuf) {
                alloc->free(alloc->ctx, buf, cap);
                return HU_ERR_OUT_OF_MEMORY;
            }
            buf = nbuf;
            cap = new_cap;
        }
        unsigned char ch;
        ssize_t n = read(c->read_fd, &ch, 1);
        if (n <= 0)
            break;
        if (ch == '\n')
            break;
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return HU_OK;
#else
    (void)c;
    (void)alloc;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static void stdio_close(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    stdio_ctx_t *c = (stdio_ctx_t *)ctx;
#ifdef HU_GATEWAY_POSIX
    if (c->child_pid >= 0) {
        (void)kill(c->child_pid, SIGTERM);
        (void)waitpid(c->child_pid, NULL, 0);
        c->child_pid = -1;
    }
    if (c->read_fd >= 0) {
        (void)close(c->read_fd);
        c->read_fd = -1;
    }
    if (c->write_fd >= 0) {
        (void)close(c->write_fd);
        c->write_fd = -1;
    }
    if (c->argv) {
        for (size_t i = 1; i < c->argv_count; i++)
            alloc->free(alloc->ctx, c->argv[i], strlen(c->argv[i]) + 1);
        alloc->free(alloc->ctx, c->argv, (c->argv_count + 1) * sizeof(char *));
        c->argv = NULL;
    }
    if (c->command) {
        alloc->free(alloc->ctx, c->command, strlen(c->command) + 1);
        c->command = NULL;
    }
#endif
    alloc->free(alloc->ctx, ctx, sizeof(stdio_ctx_t));
}

hu_error_t hu_mcp_transport_stdio_create(hu_allocator_t *alloc, int read_fd, int write_fd,
                                         hu_mcp_transport_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
#ifdef HU_GATEWAY_POSIX
    stdio_ctx_t *c = (stdio_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    c->read_fd = read_fd;
    c->write_fd = write_fd;
    c->child_pid = -1;
    c->command = NULL;
    c->argv = NULL;
    c->argv_count = 0;
#else
    (void)read_fd;
    (void)write_fd;
    stdio_ctx_t *c = (stdio_ctx_t *)alloc->alloc(alloc->ctx, sizeof(stdio_ctx_t));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    c->read_fd = read_fd;
    c->write_fd = write_fd;
#endif
    out->ctx = c;
    out->send = stdio_send;
    out->recv = stdio_recv;
    out->close = stdio_close;
    return HU_OK;
}

#if defined(HU_GATEWAY_POSIX)
hu_error_t hu_mcp_transport_stdio_create_from_command(hu_allocator_t *alloc, const char *command,
                                                      const char *const *args, size_t args_count,
                                                      hu_mcp_transport_t *out) {
    if (!alloc || !out || !command || !command[0])
        return HU_ERR_INVALID_ARGUMENT;

    stdio_ctx_t *c = (stdio_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;

    size_t cmd_len = strlen(command);
    c->command = (char *)alloc->alloc(alloc->ctx, cmd_len + 1);
    if (!c->command) {
        alloc->free(alloc->ctx, c, sizeof(*c));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(c->command, command, cmd_len + 1);

    c->argv_count = 1 + args_count;
    c->argv = (char **)alloc->alloc(alloc->ctx, (c->argv_count + 1) * sizeof(char *));
    if (!c->argv) {
        alloc->free(alloc->ctx, c->command, cmd_len + 1);
        alloc->free(alloc->ctx, c, sizeof(*c));
        return HU_ERR_OUT_OF_MEMORY;
    }
    c->argv[0] = c->command;
    for (size_t i = 0; i < args_count; i++) {
        size_t alen = args[i] ? strlen(args[i]) : 0;
        c->argv[i + 1] = (char *)alloc->alloc(alloc->ctx, alen + 1);
        if (!c->argv[i + 1]) {
            for (size_t j = 0; j < i; j++)
                alloc->free(alloc->ctx, c->argv[j + 1], strlen(c->argv[j + 1]) + 1);
            alloc->free(alloc->ctx, c->argv, (c->argv_count + 1) * sizeof(char *));
            alloc->free(alloc->ctx, c->command, cmd_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->argv[i + 1], args[i] ? args[i] : "", alen + 1);
    }
    c->argv[c->argv_count] = NULL;

    c->read_fd = -1;
    c->write_fd = -1;
    c->child_pid = -1;

    out->ctx = c;
    out->send = stdio_send;
    out->recv = stdio_recv;
    out->close = stdio_close;
    return HU_OK;
}

hu_error_t hu_mcp_transport_stdio_start(hu_allocator_t *alloc, hu_mcp_transport_t *t) {
    if (!alloc || !t || !t->ctx)
        return HU_ERR_INVALID_ARGUMENT;

    stdio_ctx_t *c = (stdio_ctx_t *)t->ctx;
    if (!c->command || !c->argv)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST != 0
    (void)alloc;
    (void)c;
    return HU_ERR_NOT_SUPPORTED;
#else
    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) != 0)
        return HU_ERR_IO;
    if (pipe(stdout_pipe) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return HU_ERR_IO;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return HU_ERR_IO;
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        execvp(c->command, (char *const *)c->argv);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    c->read_fd = stdout_pipe[0];
    c->write_fd = stdin_pipe[1];
    c->child_pid = (int)pid;
    return HU_OK;
#endif
}
#else
hu_error_t hu_mcp_transport_stdio_create_from_command(hu_allocator_t *alloc, const char *command,
                                                      const char *const *args, size_t args_count,
                                                      hu_mcp_transport_t *out) {
    (void)alloc;
    (void)command;
    (void)args;
    (void)args_count;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_mcp_transport_stdio_start(hu_allocator_t *alloc, hu_mcp_transport_t *t) {
    (void)alloc;
    (void)t;
    return HU_ERR_NOT_SUPPORTED;
}
#endif /* HU_GATEWAY_POSIX */

void hu_mcp_transport_destroy(hu_mcp_transport_t *t, hu_allocator_t *alloc) {
    if (!t || !alloc)
        return;
    if (t->close && t->ctx) {
        t->close(t->ctx, alloc);
    }
    t->ctx = NULL;
    t->send = NULL;
    t->recv = NULL;
    t->close = NULL;
}
