#ifndef SC_IDENTITY_H
#define SC_IDENTITY_H

#include "core/allocator.h"
#include "core/error.h"
#include <stddef.h>
#include <stdbool.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Identity — map channel user IDs to unified identity, permission levels
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_permission_level {
    SC_PERM_BLOCKED,
    SC_PERM_VIEWER,
    SC_PERM_USER,
    SC_PERM_ADMIN,
} sc_permission_level_t;

#define SC_IDENTITY_CHANNEL_LEN 32
#define SC_IDENTITY_USER_LEN 128
#define SC_IDENTITY_UNIFIED_LEN 256

/* Unified identity key: "channel:account:user" or "channel:user" */
typedef struct sc_identity {
    char channel[SC_IDENTITY_CHANNEL_LEN];
    char user_id[SC_IDENTITY_USER_LEN];  /* channel-specific ID */
    char unified_id[SC_IDENTITY_UNIFIED_LEN];  /* global ID for lookups */
    sc_permission_level_t level;
} sc_identity_t;

/* Build unified_id from channel + user_id. Optional account_id for multi-account. */
void sc_identity_build_unified(sc_identity_t *identity,
                               const char *channel,
                               const char *account_id,
                               const char *user_id);

/* Resolve permission level (e.g. from allowlist). Returns SC_PERM_USER by default. */
sc_permission_level_t sc_identity_resolve_level(const char *unified_id,
                                                const char *const *allowlist,
                                                size_t allowlist_count);

/* Check if identity has at least required level. */
bool sc_identity_has_permission(sc_permission_level_t have,
                                sc_permission_level_t required);

#endif /* SC_IDENTITY_H */
