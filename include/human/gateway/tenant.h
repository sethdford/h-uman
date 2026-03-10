#ifndef HU_GATEWAY_TENANT_H
#define HU_GATEWAY_TENANT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_tenant_role {
    HU_TENANT_ROLE_ADMIN = 0,
    HU_TENANT_ROLE_USER = 1,
    HU_TENANT_ROLE_READONLY = 2
} hu_tenant_role_t;

typedef struct hu_tenant {
    char user_id[256];
    char display_name[256];
    hu_tenant_role_t role;
    uint32_t rate_limit_rpm;
    uint64_t usage_quota_tokens;
    uint64_t usage_current_tokens;
} hu_tenant_t;

typedef struct hu_tenant_store hu_tenant_store_t;

hu_error_t hu_tenant_store_init(hu_allocator_t *alloc, hu_tenant_store_t **out);
void hu_tenant_store_destroy(hu_tenant_store_t *store);

hu_error_t hu_tenant_create(hu_tenant_store_t *store, const hu_tenant_t *tenant);
hu_error_t hu_tenant_get(hu_tenant_store_t *store, const char *user_id, hu_tenant_t *out);
hu_error_t hu_tenant_update(hu_tenant_store_t *store, const hu_tenant_t *tenant);
hu_error_t hu_tenant_delete(hu_tenant_store_t *store, const char *user_id);
hu_error_t hu_tenant_list(hu_tenant_store_t *store, hu_tenant_t *out, size_t max, size_t *count);

hu_error_t hu_tenant_increment_usage(hu_tenant_store_t *store, const char *user_id,
                                     uint64_t tokens);
bool hu_tenant_check_quota(hu_tenant_store_t *store, const char *user_id);
bool hu_tenant_check_rate_limit(hu_tenant_store_t *store, const char *user_id);

#endif
