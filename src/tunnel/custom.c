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

#define SC_PORT_PLACEHOLDER "{port}"
#define SC_CMD_MAX 2048

typedef struct sc_custom_tunnel {
    char *public_url;
    bool running;
    void *child_handle; /* FILE* from popen */
    sc_allocator_t *alloc;
    char *command_template; /* owned, with {port} placeholder */
    size_t command_template_len;
} sc_custom_tunnel_t;

static void impl_stop(void *ctx);

static sc_tunnel_error_t impl_start(void *ctx, uint16_t local_port,
    char **public_url_out, size_t *url_len) {
    sc_custom_tunnel_t *self = (sc_custom_tunnel_t *)ctx;

    impl_stop(ctx);

#if SC_IS_TEST
    (void)local_port;
    char *mock = sc_strdup(self->alloc, "https://custom-mock.example.net");
    if (!mock) return SC_TUNNEL_ERR_START_FAILED;
    if (self->public_url)
        self->alloc->free(self->alloc->ctx, self->public_url, strlen(self->public_url) + 1);
    self->public_url = mock;
    self->running = true;
    *public_url_out = mock;
    *url_len = strlen(mock);
    return SC_TUNNEL_ERR_OK;
#else
    if (!self->command_template || self->command_template_len == 0)
        return SC_TUNNEL_ERR_INVALID_COMMAND;

    /* Substitute {port} with actual port */
    char cmd[SC_CMD_MAX];
    const char *placeholder = strstr(self->command_template, SC_PORT_PLACEHOLDER);
    if (!placeholder) {
        /* No placeholder: use template as-is (user may have fixed port) */
        size_t copy_len = self->command_template_len;
        if (copy_len >= sizeof(cmd)) copy_len = sizeof(cmd) - 1;
        memcpy(cmd, self->command_template, copy_len);
        cmd[copy_len] = '\0';
    } else {
        size_t before = (size_t)(placeholder - self->command_template);
        size_t after = self->command_template_len - before - strlen(SC_PORT_PLACEHOLDER);
        if (before + 8 + after >= sizeof(cmd)) return SC_TUNNEL_ERR_INVALID_COMMAND;
        memcpy(cmd, self->command_template, before);
        int n = snprintf(cmd + before, sizeof(cmd) - before, "%u%s",
            (unsigned)local_port, placeholder + strlen(SC_PORT_PLACEHOLDER));
        if (n < 0 || (size_t)n >= sizeof(cmd) - before)
            return SC_TUNNEL_ERR_INVALID_COMMAND;
    }

    FILE *f = sc_popen(cmd, "r");
    if (!f) return SC_TUNNEL_ERR_PROCESS_SPAWN;

    self->child_handle = f;
    self->running = true;

    /* Read first line of stdout as URL */
    char line[1024];
    char *found_url = NULL;
    if (fgets(line, sizeof(line), f)) {
        /* Trim trailing newline and whitespace */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';
        if (len > 0)
            found_url = sc_strndup(self->alloc, line, len);
    }

    if (!found_url || strlen(found_url) == 0) {
        if (found_url) self->alloc->free(self->alloc->ctx, found_url, strlen(found_url) + 1);
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
#endif
}

static void impl_stop(void *ctx) {
    sc_custom_tunnel_t *self = (sc_custom_tunnel_t *)ctx;
    if (self->child_handle) {
        sc_pclose((FILE *)self->child_handle);
        self->child_handle = NULL;
    }
    self->running = false;
}

static const char *impl_public_url(void *ctx) {
    sc_custom_tunnel_t *self = (sc_custom_tunnel_t *)ctx;
    return self->public_url ? self->public_url : "";
}

static const char *impl_provider_name(void *ctx) {
    (void)ctx;
    return "custom";
}

static bool impl_is_running(void *ctx) {
    sc_custom_tunnel_t *self = (sc_custom_tunnel_t *)ctx;
    return self->running && self->child_handle != NULL;
}

static void impl_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_custom_tunnel_t *self = (sc_custom_tunnel_t *)ctx;
    impl_stop(ctx);
    if (self->public_url) {
        alloc->free(alloc->ctx, self->public_url, strlen(self->public_url) + 1);
        self->public_url = NULL;
    }
    if (self->command_template) {
        alloc->free(alloc->ctx, self->command_template, self->command_template_len + 1);
        self->command_template = NULL;
    }
    alloc->free(alloc->ctx, self, sizeof(sc_custom_tunnel_t));
}

static const sc_tunnel_vtable_t custom_vtable = {
    .start = impl_start,
    .stop = impl_stop,
    .deinit = impl_deinit,
    .public_url = impl_public_url,
    .provider_name = impl_provider_name,
    .is_running = impl_is_running,
};

sc_tunnel_t sc_custom_tunnel_create(sc_allocator_t *alloc,
    const char *command_template, size_t command_template_len) {
    sc_custom_tunnel_t *self = (sc_custom_tunnel_t *)alloc->alloc(alloc->ctx,
        sizeof(sc_custom_tunnel_t));
    if (!self) return (sc_tunnel_t){ .ctx = NULL, .vtable = NULL };
    self->public_url = NULL;
    self->running = false;
    self->child_handle = NULL;
    self->alloc = alloc;
    self->command_template = NULL;
    self->command_template_len = 0;
    if (command_template && command_template_len > 0) {
        self->command_template = sc_strndup(alloc, command_template, command_template_len);
        if (self->command_template)
            self->command_template_len = strlen(self->command_template);
    }
    return (sc_tunnel_t){
        .ctx = self,
        .vtable = &custom_vtable,
    };
}
