#include "seaclaw/tunnel.h"
#include <stdint.h>
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#define sc_popen(cmd, mode) _popen(cmd, mode)
#define sc_pclose(f) _pclose(f)
#else
#include <unistd.h>
#define sc_popen(cmd, mode) popen(cmd, mode)
#define sc_pclose(f) pclose(f)
#endif

static void impl_stop(void *ctx);

typedef struct sc_ngrok_tunnel {
    char *public_url;
    bool running;
    void *child_handle;
    sc_allocator_t *alloc;
} sc_ngrok_tunnel_t;

static sc_tunnel_error_t impl_start(void *ctx, uint16_t local_port,
    char **public_url_out, size_t *url_len) {
    sc_ngrok_tunnel_t *self = (sc_ngrok_tunnel_t *)ctx;

    impl_stop(ctx);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ngrok http %u 2>/dev/null", (unsigned)local_port);

    FILE *f = sc_popen(cmd, "r");
    if (!f) return SC_TUNNEL_ERR_PROCESS_SPAWN;

    self->child_handle = f;
    self->running = true;

    /* Read output to find ngrok URL - typically contains ngrok-free.app or ngrok.io */
    char line[1024];
    char *found_url = NULL;
    for (int i = 0; i < 50 && fgets(line, sizeof(line), f); i++) {
        const char *ng = strstr(line, "ngrok-free.app");
        if (!ng) ng = strstr(line, "ngrok.io");
        if (ng) {
            const char *start = line;
            for (const char *p = ng; p > line; p--) {
                if (strncmp(p, "https://", 8) == 0 || strncmp(p, "http://", 7) == 0) {
                    start = p;
                    break;
                }
            }
            size_t urllen = 0;
            for (const char *p = start; *p && *p != ' ' && *p != '\n' && *p != '\r' && *p != '>' && urllen < 200; p++, urllen++)
                ;
            found_url = sc_strndup(self->alloc, start, urllen);
            break;
        }
    }

    if (!found_url) {
        sc_pclose(f);
        self->child_handle = NULL;
        self->running = false;
        return SC_TUNNEL_ERR_URL_NOT_FOUND;
    }

    if (self->public_url)
        self->alloc->free(self->alloc->ctx, self->public_url, strlen(self->public_url) + 1);
    self->public_url = found_url;
    *public_url_out = found_url;
    *url_len = strlen(found_url);
    return SC_TUNNEL_ERR_OK;
}

static void impl_stop(void *ctx) {
    sc_ngrok_tunnel_t *self = (sc_ngrok_tunnel_t *)ctx;
    if (self->child_handle) {
        sc_pclose((FILE *)self->child_handle);
        self->child_handle = NULL;
    }
    self->running = false;
}

static const char *impl_public_url(void *ctx) {
    sc_ngrok_tunnel_t *self = (sc_ngrok_tunnel_t *)ctx;
    return self->public_url ? self->public_url : "";
}

static const char *impl_provider_name(void *ctx) {
    (void)ctx;
    return "ngrok";
}

static bool impl_is_running(void *ctx) {
    sc_ngrok_tunnel_t *self = (sc_ngrok_tunnel_t *)ctx;
    return self->running && self->child_handle != NULL;
}

static void impl_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_ngrok_tunnel_t *self = (sc_ngrok_tunnel_t *)ctx;
    impl_stop(ctx);
    if (self->public_url) {
        alloc->free(alloc->ctx, self->public_url, strlen(self->public_url) + 1);
        self->public_url = NULL;
    }
    alloc->free(alloc->ctx, self, sizeof(sc_ngrok_tunnel_t));
}

static const sc_tunnel_vtable_t ngrok_vtable = {
    .start = impl_start,
    .stop = impl_stop,
    .deinit = impl_deinit,
    .public_url = impl_public_url,
    .provider_name = impl_provider_name,
    .is_running = impl_is_running,
};

sc_tunnel_t sc_ngrok_tunnel_create(sc_allocator_t *alloc,
    const char *auth_token, size_t auth_token_len,
    const char *domain, size_t domain_len) {
    sc_ngrok_tunnel_t *self = (sc_ngrok_tunnel_t *)alloc->alloc(alloc->ctx,
        sizeof(sc_ngrok_tunnel_t));
    if (!self) return (sc_tunnel_t){ .ctx = NULL, .vtable = NULL };
    self->public_url = NULL;
    self->running = false;
    self->child_handle = NULL;
    self->alloc = alloc;
    (void)auth_token;
    (void)auth_token_len;
    (void)domain;
    (void)domain_len;
    return (sc_tunnel_t){
        .ctx = self,
        .vtable = &ngrok_vtable,
    };
}
