#include "seaclaw/tunnel.h"
#include <stdint.h>
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <process.h>
#define sc_popen(cmd, mode) _popen(cmd, mode)
#define sc_pclose(f) _pclose(f)
#else
#include <unistd.h>
#include <sys/wait.h>
#define sc_popen(cmd, mode) popen(cmd, mode)
#define sc_pclose(f) pclose(f)
#endif

typedef struct sc_cloudflare_tunnel {
    char *public_url;
    bool running;
    void *child_handle;
    sc_allocator_t *alloc;
} sc_cloudflare_tunnel_t;

static sc_tunnel_error_t impl_start(void *ctx, uint16_t local_port,
    char **public_url_out, size_t *url_len) {
    sc_cloudflare_tunnel_t *self = (sc_cloudflare_tunnel_t *)ctx;

    if (self->child_handle) {
        sc_pclose((FILE *)self->child_handle);
        self->child_handle = NULL;
    }
    self->running = false;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cloudflared tunnel --url http://localhost:%u 2>/dev/null",
        (unsigned)local_port);

    FILE *f = sc_popen(cmd, "r");
    if (!f) return SC_TUNNEL_ERR_PROCESS_SPAWN;

    self->child_handle = f;
    self->running = true;

    char line[512];
    char *found_url = NULL;
    while (fgets(line, sizeof(line), f) && !found_url) {
        const char *trycf = strstr(line, "trycloudflare.com");
        if (trycf) {
            const char *start = line;
            for (const char *p = trycf; p > line; p--) {
                if (strncmp(p, "https://", 8) == 0 || strncmp(p, "http://", 7) == 0) {
                    start = p;
                    break;
                }
            }
            size_t urllen = 0;
            for (const char *p = start; *p && *p != ' ' && *p != '\n' && *p != '\r' && urllen < 200; p++, urllen++)
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
    sc_cloudflare_tunnel_t *self = (sc_cloudflare_tunnel_t *)ctx;
    if (self->child_handle) {
        sc_pclose((FILE *)self->child_handle);
        self->child_handle = NULL;
    }
    self->running = false;
}

static const char *impl_public_url(void *ctx) {
    sc_cloudflare_tunnel_t *self = (sc_cloudflare_tunnel_t *)ctx;
    return self->public_url ? self->public_url : "";
}

static const char *impl_provider_name(void *ctx) {
    (void)ctx;
    return "cloudflare";
}

static bool impl_is_running(void *ctx) {
    sc_cloudflare_tunnel_t *self = (sc_cloudflare_tunnel_t *)ctx;
    return self->running && self->child_handle != NULL;
}

static const sc_tunnel_vtable_t cloudflare_vtable = {
    .start = impl_start,
    .stop = impl_stop,
    .public_url = impl_public_url,
    .provider_name = impl_provider_name,
    .is_running = impl_is_running,
};

sc_tunnel_t sc_cloudflare_tunnel_create(sc_allocator_t *alloc, const char *token, size_t token_len) {
    sc_cloudflare_tunnel_t *self = (sc_cloudflare_tunnel_t *)alloc->alloc(alloc->ctx,
        sizeof(sc_cloudflare_tunnel_t));
    if (!self) return (sc_tunnel_t){ .ctx = NULL, .vtable = NULL };
    self->public_url = NULL;
    self->running = false;
    self->child_handle = NULL;
    self->alloc = alloc;
    (void)token;
    (void)token_len;
    return (sc_tunnel_t){
        .ctx = self,
        .vtable = &cloudflare_vtable,
    };
}
