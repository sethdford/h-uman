#include "human/core/allocator.h"
#include "human/tunnel.h"
#include <string.h>

static const char *tunnel_err_strings[] = {
    [HU_TUNNEL_ERR_OK] = "ok",
    [HU_TUNNEL_ERR_START_FAILED] = "tunnel start failed",
    [HU_TUNNEL_ERR_PROCESS_SPAWN] = "process spawn failed",
    [HU_TUNNEL_ERR_URL_NOT_FOUND] = "public URL not found",
    [HU_TUNNEL_ERR_TIMEOUT] = "timeout",
    [HU_TUNNEL_ERR_INVALID_COMMAND] = "invalid command",
    [HU_TUNNEL_ERR_NOT_IMPLEMENTED] = "not implemented",
};

const char *hu_tunnel_error_string(hu_tunnel_error_t err) {
    if ((unsigned)err < sizeof(tunnel_err_strings) / sizeof(tunnel_err_strings[0]))
        return tunnel_err_strings[err];
    return "unknown tunnel error";
}

hu_tunnel_t hu_tunnel_create(hu_allocator_t *alloc, const hu_tunnel_config_t *config) {
    if (!alloc)
        return (hu_tunnel_t){.ctx = NULL, .vtable = NULL};
    if (!config)
        return hu_none_tunnel_create(alloc);

    switch (config->provider) {
    case HU_TUNNEL_NONE:
        return hu_none_tunnel_create(alloc);
#ifdef HU_HAS_TUNNELS
    case HU_TUNNEL_CLOUDFLARE:
        return hu_cloudflare_tunnel_create(alloc, config->cloudflare_token,
                                           config->cloudflare_token_len);
    case HU_TUNNEL_NGROK:
        return hu_ngrok_tunnel_create(alloc, config->ngrok_auth_token, config->ngrok_auth_token_len,
                                      config->ngrok_domain, config->ngrok_domain_len);
    case HU_TUNNEL_TAILSCALE:
        return hu_tailscale_tunnel_create(alloc);
    case HU_TUNNEL_CUSTOM:
        return hu_custom_tunnel_create(alloc, config->custom_start_cmd,
                                       config->custom_start_cmd_len);
#endif
    default:
        return hu_none_tunnel_create(alloc);
    }
}
