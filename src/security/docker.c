#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/core/error.h"

#define SC_DOCKER_IMAGE_MAX 127
#define SC_DOCKER_MOUNT_MAX 2048
#include <string.h>
#include <stdio.h>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/wait.h>
#endif

/* Use types from sandbox_internal.h */
/* sc_docker_ctx_t from sandbox_internal.h */

static sc_error_t docker_wrap(void *ctx, const char *const *argv, size_t argc,
    const char **buf, size_t buf_count, size_t *out_count) {
    sc_docker_ctx_t *dk = (sc_docker_ctx_t *)ctx;
    /* docker run --rm --memory 512m --cpus 1.0 --network none
       -v WORKSPACE:WORKSPACE IMAGE <argv...> */
    const char *prefix[] = {
        "docker", "run", "--rm",
        "--memory", "512m", "--cpus", "1.0",
        "--network", "none",
        "-v",
    };
    const size_t prefix_len = sizeof(prefix) / sizeof(prefix[0]);
    const size_t total = prefix_len + 2 + argc;  /* +2 for mount_arg and image */

    if (!buf || !out_count) return SC_ERR_INVALID_ARGUMENT;
    if (buf_count < total) return SC_ERR_INVALID_ARGUMENT;

    size_t i = 0;
    for (; i < prefix_len; i++)
        buf[i] = prefix[i];
    buf[i++] = dk->mount_arg;
    buf[i++] = dk->image;
    for (size_t j = 0; j < argc; j++)
        buf[i++] = argv[j];
    *out_count = total;
    return SC_OK;
}

static bool docker_available(void *ctx) {
    (void)ctx;
#if SC_IS_TEST
    return false;
#elif defined(__linux__) || defined(__APPLE__)
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execlp("docker", "docker", "--version", (char *)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
    return false;
#endif
}

static const char *docker_name(void *ctx) {
    (void)ctx;
    return "docker";
}

static const char *docker_desc(void *ctx) {
    (void)ctx;
    return "Docker container isolation (requires docker)";
}

static const sc_sandbox_vtable_t docker_vtable = {
    .wrap_command = docker_wrap,
    .apply = NULL,
    .is_available = docker_available,
    .name = docker_name,
    .description = docker_desc,
};

sc_sandbox_t sc_docker_sandbox_get(sc_docker_ctx_t *ctx) {
    sc_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &docker_vtable,
    };
    return sb;
}

void sc_docker_sandbox_init(sc_docker_ctx_t *ctx, const char *workspace_dir,
    const char *image, void *alloc_ctx,
    void *(*alloc_fn)(void *, size_t),
    void (*free_fn)(void *, void *, size_t)) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc_ctx = alloc_ctx;
    ctx->alloc_fn = alloc_fn;
    ctx->free_fn = free_fn;

    const char *img = image && image[0] ? image : "alpine:latest";
    size_t ilen = strlen(img);
    if (ilen > SC_DOCKER_IMAGE_MAX) ilen = SC_DOCKER_IMAGE_MAX;
    memcpy(ctx->image, img, ilen);
    ctx->image[ilen] = '\0';

    if (workspace_dir && workspace_dir[0]) {
        size_t wlen = strlen(workspace_dir);
        if (wlen > SC_DOCKER_MOUNT_MAX) wlen = SC_DOCKER_MOUNT_MAX;
        int n = snprintf(ctx->mount_arg, sizeof(ctx->mount_arg),
            "%.*s:%.*s", (int)wlen, workspace_dir, (int)wlen, workspace_dir);
        if (n > 0 && (size_t)n < sizeof(ctx->mount_arg))
            ctx->mount_len = (size_t)n;
    }
}
