#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/tunnel.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define hu_popen(cmd, mode) _popen(cmd, mode)
#define hu_pclose(f)        _pclose(f)
#else
#include <unistd.h>
#define hu_popen(cmd, mode) popen(cmd, mode)
#define hu_pclose(f)        pclose(f)
#endif

static void impl_stop(void *ctx);

typedef struct hu_ngrok_tunnel {
    char *public_url;
    bool running;
    void *child_handle;
    hu_allocator_t *alloc;
} hu_ngrok_tunnel_t;

static hu_tunnel_error_t impl_start(void *ctx, uint16_t local_port, char **public_url_out,
                                    size_t *url_len) {
    hu_ngrok_tunnel_t *self = (hu_ngrok_tunnel_t *)ctx;

    impl_stop(ctx);

#if HU_IS_TEST
    (void)local_port;
    char *mock = hu_strdup(self->alloc, "https://test-ngrok.ngrok-free.app");
    if (!mock)
        return HU_TUNNEL_ERR_START_FAILED;
    if (self->public_url)
        self->alloc->free(self->alloc->ctx, self->public_url, strlen(self->public_url) + 1);
    self->public_url = mock;
    self->running = true;
    *public_url_out = mock;
    *url_len = strlen(mock);
    return HU_TUNNEL_ERR_OK;
#else
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ngrok http %u 2>/dev/null", (unsigned)local_port);

    FILE *f = hu_popen(cmd, "r");
    if (!f)
        return HU_TUNNEL_ERR_PROCESS_SPAWN;

    self->child_handle = f;
    self->running = true;

    /* Read output to find ngrok URL - typically contains ngrok-free.app or ngrok.io */
    char line[1024];
    char *found_url = NULL;
    for (int i = 0; i < 50 && fgets(line, sizeof(line), f); i++) {
        const char *ng = strstr(line, "ngrok-free.app");
        if (!ng)
            ng = strstr(line, "ngrok.io");
        if (ng) {
            const char *start = line;
            for (const char *p = ng; p > line; p--) {
                if (strncmp(p, "https://", 8) == 0 || strncmp(p, "http://", 7) == 0) {
                    start = p;
                    break;
                }
            }
            size_t urllen = 0;
            for (const char *p = start;
                 *p && *p != ' ' && *p != '\n' && *p != '\r' && *p != '>' && urllen < 200;
                 p++, urllen++)
                ;
            found_url = hu_strndup(self->alloc, start, urllen);
            break;
        }
    }

    if (!found_url) {
        hu_pclose(f);
        self->child_handle = NULL;
        self->running = false;
        return HU_TUNNEL_ERR_URL_NOT_FOUND;
    }

    if (self->public_url)
        self->alloc->free(self->alloc->ctx, self->public_url, strlen(self->public_url) + 1);
    self->public_url = found_url;
    *public_url_out = found_url;
    *url_len = strlen(found_url);
    return HU_TUNNEL_ERR_OK;
#endif /* HU_IS_TEST */
}

static void impl_stop(void *ctx) {
    hu_ngrok_tunnel_t *self = (hu_ngrok_tunnel_t *)ctx;
    if (self->child_handle) {
        hu_pclose((FILE *)self->child_handle);
        self->child_handle = NULL;
    }
    self->running = false;
}

static const char *impl_public_url(void *ctx) {
    hu_ngrok_tunnel_t *self = (hu_ngrok_tunnel_t *)ctx;
    return self->public_url ? self->public_url : "";
}

static const char *impl_provider_name(void *ctx) {
    (void)ctx;
    return "ngrok";
}

static bool impl_is_running(void *ctx) {
    hu_ngrok_tunnel_t *self = (hu_ngrok_tunnel_t *)ctx;
    return self->running && self->child_handle != NULL;
}

static void impl_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_ngrok_tunnel_t *self = (hu_ngrok_tunnel_t *)ctx;
    impl_stop(ctx);
    if (self->public_url) {
        alloc->free(alloc->ctx, self->public_url, strlen(self->public_url) + 1);
        self->public_url = NULL;
    }
    alloc->free(alloc->ctx, self, sizeof(hu_ngrok_tunnel_t));
}

static const hu_tunnel_vtable_t ngrok_vtable = {
    .start = impl_start,
    .stop = impl_stop,
    .deinit = impl_deinit,
    .public_url = impl_public_url,
    .provider_name = impl_provider_name,
    .is_running = impl_is_running,
};

hu_tunnel_t hu_ngrok_tunnel_create(hu_allocator_t *alloc, const char *auth_token,
                                   size_t auth_token_len, const char *domain, size_t domain_len) {
    hu_ngrok_tunnel_t *self =
        (hu_ngrok_tunnel_t *)alloc->alloc(alloc->ctx, sizeof(hu_ngrok_tunnel_t));
    if (!self)
        return (hu_tunnel_t){.ctx = NULL, .vtable = NULL};
    self->public_url = NULL;
    self->running = false;
    self->child_handle = NULL;
    self->alloc = alloc;
    (void)auth_token;
    (void)auth_token_len;
    (void)domain;
    (void)domain_len;
    return (hu_tunnel_t){
        .ctx = self,
        .vtable = &ngrok_vtable,
    };
}
