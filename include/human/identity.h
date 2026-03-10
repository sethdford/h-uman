#ifndef HU_IDENTITY_H
#define HU_IDENTITY_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Identity — map channel user IDs to unified identity, permission levels
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_permission_level {
    HU_PERM_BLOCKED,
    HU_PERM_VIEWER,
    HU_PERM_USER,
    HU_PERM_ADMIN,
} hu_permission_level_t;

#define HU_IDENTITY_CHANNEL_LEN 32
#define HU_IDENTITY_USER_LEN    128
#define HU_IDENTITY_UNIFIED_LEN 256

/* Unified identity key: "channel:account:user" or "channel:user" */
typedef struct hu_identity {
    char channel[HU_IDENTITY_CHANNEL_LEN];
    char user_id[HU_IDENTITY_USER_LEN];       /* channel-specific ID */
    char unified_id[HU_IDENTITY_UNIFIED_LEN]; /* global ID for lookups */
    hu_permission_level_t level;
} hu_identity_t;

/* Build unified_id from channel + user_id. Optional account_id for multi-account. */
void hu_identity_build_unified(hu_identity_t *identity, const char *channel, const char *account_id,
                               const char *user_id);

/* Resolve permission level (e.g. from allowlist). Returns HU_PERM_USER by default. */
hu_permission_level_t hu_identity_resolve_level(const char *unified_id,
                                                const char *const *allowlist,
                                                size_t allowlist_count);

/* Check if identity has at least required level. */
bool hu_identity_has_permission(hu_permission_level_t have, hu_permission_level_t required);

#endif /* HU_IDENTITY_H */
