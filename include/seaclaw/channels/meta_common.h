#ifndef SC_META_COMMON_H
#define SC_META_COMMON_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* Verify Meta webhook HMAC-SHA256 signature (X-Hub-Signature-256: sha256=<hex>). */
bool sc_meta_verify_webhook(const char *body, size_t body_len, const char *signature,
                            const char *app_secret);

/* Send a message via Meta Graph API (Messenger or Instagram). */
sc_error_t sc_meta_graph_send(sc_allocator_t *alloc, const char *access_token,
                              size_t access_token_len, const char *endpoint, size_t endpoint_len,
                              const char *json_body, size_t json_body_len);

/* Build a Graph API URL: https://graph.facebook.com/v21.0/<path> */
sc_error_t sc_meta_graph_url(sc_allocator_t *alloc, const char *path, size_t path_len,
                             char **out_url, size_t *out_url_len);

#endif /* SC_META_COMMON_H */
