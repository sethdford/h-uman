#include "seaclaw/tunnel.h"
#include "seaclaw/core/allocator.h"
#include <string.h>

static const char *tunnel_err_strings[] = {
    [SC_TUNNEL_ERR_OK] = "ok",
    [SC_TUNNEL_ERR_START_FAILED] = "tunnel start failed",
    [SC_TUNNEL_ERR_PROCESS_SPAWN] = "process spawn failed",
    [SC_TUNNEL_ERR_URL_NOT_FOUND] = "public URL not found",
    [SC_TUNNEL_ERR_TIMEOUT] = "timeout",
    [SC_TUNNEL_ERR_INVALID_COMMAND] = "invalid command",
    [SC_TUNNEL_ERR_NOT_IMPLEMENTED] = "not implemented",
};

const char *sc_tunnel_error_string(sc_tunnel_error_t err) {
    if ((unsigned)err < sizeof(tunnel_err_strings) / sizeof(tunnel_err_strings[0]))
        return tunnel_err_strings[err];
    return "unknown tunnel error";
}

sc_tunnel_t sc_tunnel_create(sc_allocator_t *alloc, const sc_tunnel_config_t *config) {
    if (!alloc) return (sc_tunnel_t){ .ctx = NULL, .vtable = NULL };
    if (!config) return sc_none_tunnel_create(alloc);

    switch (config->provider) {
        case SC_TUNNEL_NONE:
            return sc_none_tunnel_create(alloc);
        case SC_TUNNEL_CLOUDFLARE:
            return sc_cloudflare_tunnel_create(alloc,
                config->cloudflare_token, config->cloudflare_token_len);
        case SC_TUNNEL_NGROK:
            return sc_ngrok_tunnel_create(alloc,
                config->ngrok_auth_token, config->ngrok_auth_token_len,
                config->ngrok_domain, config->ngrok_domain_len);
        case SC_TUNNEL_TAILSCALE:
            return sc_tailscale_tunnel_create(alloc);
        case SC_TUNNEL_CUSTOM:
            return sc_custom_tunnel_create(alloc,
                config->custom_start_cmd, config->custom_start_cmd_len);
        default:
            return sc_none_tunnel_create(alloc);
    }
}
