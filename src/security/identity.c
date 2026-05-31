#include "human/identity.h"
#include "human/util.h"
#include <string.h>

void hu_identity_build_unified(hu_identity_t *identity, const char *channel, const char *account_id,
                               const char *user_id) {
    if (!identity)
        return;
    memset(identity, 0, sizeof(*identity));
    if (channel) {
        size_t n = strlen(channel);
        if (n >= HU_IDENTITY_CHANNEL_LEN)
            n = HU_IDENTITY_CHANNEL_LEN - 1;
        memcpy(identity->channel, channel, n);
        identity->channel[n] = '\0';
    }
    if (user_id) {
        size_t n = strlen(user_id);
        if (n >= HU_IDENTITY_USER_LEN)
            n = HU_IDENTITY_USER_LEN - 1;
        memcpy(identity->user_id, user_id, n);
        identity->user_id[n] = '\0';
    }
    /* unified: channel[:account]:user_id */
    if (channel && user_id) {
        size_t pos = 0;
        size_t cl = strlen(channel);
        if (pos + cl < HU_IDENTITY_UNIFIED_LEN) {
            memcpy(identity->unified_id, channel, cl);
            pos += cl;
        }
        if (account_id && *account_id) {
            if (pos + 1 < HU_IDENTITY_UNIFIED_LEN)
                identity->unified_id[pos++] = ':';
            size_t al = strlen(account_id);
            if (pos + al < HU_IDENTITY_UNIFIED_LEN) {
                size_t rem = HU_IDENTITY_UNIFIED_LEN - pos - 1;
                if (al > rem)
                    al = rem;
                memcpy(identity->unified_id + pos, account_id, al);
                pos += al;
            }
        }
        if (pos + 1 < HU_IDENTITY_UNIFIED_LEN)
            identity->unified_id[pos++] = ':';
        size_t ul = strlen(user_id);
        size_t rem = HU_IDENTITY_UNIFIED_LEN - pos - 1;
        if (ul > rem)
            ul = rem;
        memcpy(identity->unified_id + pos, user_id, ul);
        identity->unified_id[pos + ul] = '\0';
    }
}

hu_permission_level_t hu_identity_resolve_level(const char *unified_id,
                                                const char *const *allowlist,
                                                size_t allowlist_count) {
    if (!unified_id)
        return HU_PERM_USER;
    if (allowlist) {
        for (size_t i = 0; i < allowlist_count; i++) {
            if (allowlist[i] && hu_util_strcasecmp(unified_id, allowlist[i]) == 0)
                return HU_PERM_ADMIN; /* allowlist = admin */
        }
    }
    return HU_PERM_USER;
}

bool hu_identity_has_permission(hu_permission_level_t have, hu_permission_level_t required) {
    return (int)have >= (int)required;
}
