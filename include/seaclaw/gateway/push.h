#ifndef SC_PUSH_H
#define SC_PUSH_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum sc_push_provider {
    SC_PUSH_NONE = 0,
    SC_PUSH_FCM,
    SC_PUSH_APNS,
} sc_push_provider_t;

typedef struct sc_push_config {
    sc_push_provider_t provider;
    const char *server_key;
    size_t server_key_len;
    const char *endpoint;
} sc_push_config_t;

typedef struct sc_push_token {
    char *device_token;
    sc_push_provider_t provider;
} sc_push_token_t;

typedef struct sc_push_manager {
    sc_allocator_t *alloc;
    sc_push_config_t config;
    sc_push_token_t *tokens;
    size_t token_count;
    size_t token_cap;
} sc_push_manager_t;

sc_error_t sc_push_init(sc_push_manager_t *mgr, sc_allocator_t *alloc,
                        const sc_push_config_t *config);
void sc_push_deinit(sc_push_manager_t *mgr);

sc_error_t sc_push_register_token(sc_push_manager_t *mgr, const char *device_token,
                                  sc_push_provider_t provider);
sc_error_t sc_push_unregister_token(sc_push_manager_t *mgr, const char *device_token);

sc_error_t sc_push_send(sc_push_manager_t *mgr, const char *title, const char *body,
                        const char *data_json);
sc_error_t sc_push_send_to(sc_push_manager_t *mgr, const char *device_token, const char *title,
                           const char *body, const char *data_json);

#endif /* SC_PUSH_H */
