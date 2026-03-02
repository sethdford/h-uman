#include "seaclaw/tunnel.h"
#include <stdint.h>
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdio.h>

typedef struct sc_none_tunnel {
    char *url;
    bool running;
    sc_allocator_t *alloc;
} sc_none_tunnel_t;

static sc_tunnel_error_t impl_start(void *ctx, uint16_t local_port,
    char **public_url_out, size_t *url_len) {
    sc_none_tunnel_t *self = (sc_none_tunnel_t *)ctx;
    (void)local_port;

    if (self->url) {
        self->alloc->free(self->alloc->ctx, self->url, strlen(self->url) + 1);
        self->url = NULL;
    }

    char *u = sc_sprintf(self->alloc, "http://localhost:%u", (unsigned)local_port);
    if (!u) return SC_TUNNEL_ERR_START_FAILED;

    self->url = u;
    self->running = true;
    *public_url_out = u;
    *url_len = strlen(u);
    return SC_TUNNEL_ERR_OK;
}

static void impl_stop(void *ctx) {
    sc_none_tunnel_t *self = (sc_none_tunnel_t *)ctx;
    self->running = false;
}

static const char *impl_public_url(void *ctx) {
    sc_none_tunnel_t *self = (sc_none_tunnel_t *)ctx;
    return self->url ? self->url : "http://localhost:0";
}

static const char *impl_provider_name(void *ctx) {
    (void)ctx;
    return "none";
}

static bool impl_is_running(void *ctx) {
    sc_none_tunnel_t *self = (sc_none_tunnel_t *)ctx;
    return self->running;
}

static void impl_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_none_tunnel_t *self = (sc_none_tunnel_t *)ctx;
    if (self->url) {
        alloc->free(alloc->ctx, self->url, strlen(self->url) + 1);
        self->url = NULL;
    }
    alloc->free(alloc->ctx, self, sizeof(sc_none_tunnel_t));
}

static const sc_tunnel_vtable_t none_vtable = {
    .start = impl_start,
    .stop = impl_stop,
    .deinit = impl_deinit,
    .public_url = impl_public_url,
    .provider_name = impl_provider_name,
    .is_running = impl_is_running,
};

sc_tunnel_t sc_none_tunnel_create(sc_allocator_t *alloc) {
    sc_none_tunnel_t *self = (sc_none_tunnel_t *)alloc->alloc(alloc->ctx, sizeof(sc_none_tunnel_t));
    if (!self) return (sc_tunnel_t){ .ctx = NULL, .vtable = NULL };
    self->url = NULL;
    self->running = false;
    self->alloc = alloc;
    return (sc_tunnel_t){
        .ctx = self,
        .vtable = &none_vtable,
    };
}
