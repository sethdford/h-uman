#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/tunnel.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct hu_none_tunnel {
    char *url;
    bool running;
    hu_allocator_t *alloc;
} hu_none_tunnel_t;

static hu_tunnel_error_t impl_start(void *ctx, uint16_t local_port, char **public_url_out,
                                    size_t *url_len) {
    hu_none_tunnel_t *self = (hu_none_tunnel_t *)ctx;
    (void)local_port;

    if (self->url) {
        self->alloc->free(self->alloc->ctx, self->url, strlen(self->url) + 1);
        self->url = NULL;
    }

    char *u = hu_sprintf(self->alloc, "http://localhost:%u", (unsigned)local_port);
    if (!u)
        return HU_TUNNEL_ERR_START_FAILED;

    self->url = u;
    self->running = true;
    *public_url_out = u;
    *url_len = strlen(u);
    return HU_TUNNEL_ERR_OK;
}

static void impl_stop(void *ctx) {
    hu_none_tunnel_t *self = (hu_none_tunnel_t *)ctx;
    self->running = false;
}

static const char *impl_public_url(void *ctx) {
    hu_none_tunnel_t *self = (hu_none_tunnel_t *)ctx;
    return self->url ? self->url : "http://localhost:0";
}

static const char *impl_provider_name(void *ctx) {
    (void)ctx;
    return "none";
}

static bool impl_is_running(void *ctx) {
    hu_none_tunnel_t *self = (hu_none_tunnel_t *)ctx;
    return self->running;
}

static void impl_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_none_tunnel_t *self = (hu_none_tunnel_t *)ctx;
    if (self->url) {
        alloc->free(alloc->ctx, self->url, strlen(self->url) + 1);
        self->url = NULL;
    }
    alloc->free(alloc->ctx, self, sizeof(hu_none_tunnel_t));
}

static const hu_tunnel_vtable_t none_vtable = {
    .start = impl_start,
    .stop = impl_stop,
    .deinit = impl_deinit,
    .public_url = impl_public_url,
    .provider_name = impl_provider_name,
    .is_running = impl_is_running,
};

hu_tunnel_t hu_none_tunnel_create(hu_allocator_t *alloc) {
    hu_none_tunnel_t *self = (hu_none_tunnel_t *)alloc->alloc(alloc->ctx, sizeof(hu_none_tunnel_t));
    if (!self)
        return (hu_tunnel_t){.ctx = NULL, .vtable = NULL};
    self->url = NULL;
    self->running = false;
    self->alloc = alloc;
    return (hu_tunnel_t){
        .ctx = self,
        .vtable = &none_vtable,
    };
}
