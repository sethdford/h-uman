#ifndef HU_PUSH_H
#define HU_PUSH_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum hu_push_provider {
    HU_PUSH_NONE = 0,
    HU_PUSH_FCM,
    HU_PUSH_APNS,
} hu_push_provider_t;

typedef struct hu_push_config {
    hu_push_provider_t provider;
    const char *server_key;
    size_t server_key_len;
    const char *endpoint;
    const char *key_id;
    const char *team_id;
    const char *bundle_id;
} hu_push_config_t;

typedef struct hu_push_token {
    char *device_token;
    hu_push_provider_t provider;
} hu_push_token_t;

typedef struct hu_push_manager {
    hu_allocator_t *alloc;
    hu_push_config_t config;
    hu_push_token_t *tokens;
    size_t token_count;
    size_t token_cap;
} hu_push_manager_t;

hu_error_t hu_push_init(hu_push_manager_t *mgr, hu_allocator_t *alloc,
                        const hu_push_config_t *config);
void hu_push_deinit(hu_push_manager_t *mgr);

hu_error_t hu_push_register_token(hu_push_manager_t *mgr, const char *device_token,
                                  hu_push_provider_t provider);
hu_error_t hu_push_unregister_token(hu_push_manager_t *mgr, const char *device_token);

hu_error_t hu_push_send(hu_push_manager_t *mgr, const char *title, const char *body,
                        const char *data_json);
hu_error_t hu_push_send_to(hu_push_manager_t *mgr, const char *device_token, const char *title,
                           const char *body, const char *data_json);

#endif /* HU_PUSH_H */
