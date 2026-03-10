#ifndef HU_SECURITY_VAULT_H
#define HU_SECURITY_VAULT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

typedef struct hu_vault hu_vault_t;

/* Create a vault that stores secrets encrypted at rest.
 * vault_path: path to JSON file (default ~/.human/vault.json if NULL). */
hu_vault_t *hu_vault_create(hu_allocator_t *alloc, const char *vault_path);

/* Store a secret (encrypted with XOR using key from HUMAN_VAULT_KEY, or base64 if no key). */
hu_error_t hu_vault_set(hu_vault_t *vault, const char *key, const char *value);

/* Retrieve a secret (decrypted). out_size includes null terminator. */
hu_error_t hu_vault_get(hu_vault_t *vault, const char *key, char *out, size_t out_size);

/* Delete a secret. */
hu_error_t hu_vault_delete(hu_vault_t *vault, const char *key);

/* List all secret keys (not values). Caller allocates keys array; count set on return. */
hu_error_t hu_vault_list_keys(hu_vault_t *vault, char **keys, size_t max_keys, size_t *count);

/* Destroy vault (zeroes sensitive memory). */
void hu_vault_destroy(hu_vault_t *vault);

/* Convenience: get API key for provider. Checks: vault <provider>_api_key, env <PROVIDER>_API_KEY,
 * config api_key. config_api_key can be NULL. out must be freed by caller. */
hu_error_t hu_vault_get_api_key(hu_vault_t *vault, hu_allocator_t *alloc, const char *provider_name,
                                const char *config_api_key, char **out);

#endif /* HU_SECURITY_VAULT_H */
