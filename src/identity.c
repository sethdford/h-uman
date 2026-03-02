#include "seaclaw/identity.h"
#include "seaclaw/util.h"
#include <string.h>

void sc_identity_build_unified(sc_identity_t *identity,
                               const char *channel,
                               const char *account_id,
                               const char *user_id) {
    if (!identity) return;
    memset(identity, 0, sizeof(*identity));
    if (channel) {
        size_t n = strlen(channel);
        if (n >= SC_IDENTITY_CHANNEL_LEN) n = SC_IDENTITY_CHANNEL_LEN - 1;
        memcpy(identity->channel, channel, n);
        identity->channel[n] = '\0';
    }
    if (user_id) {
        size_t n = strlen(user_id);
        if (n >= SC_IDENTITY_USER_LEN) n = SC_IDENTITY_USER_LEN - 1;
        memcpy(identity->user_id, user_id, n);
        identity->user_id[n] = '\0';
    }
    /* unified: channel[:account]:user_id */
    if (channel && user_id) {
        size_t pos = 0;
        size_t cl = strlen(channel);
        if (pos + cl < SC_IDENTITY_UNIFIED_LEN) {
            memcpy(identity->unified_id, channel, cl);
            pos += cl;
        }
        if (account_id && *account_id) {
            if (pos + 1 < SC_IDENTITY_UNIFIED_LEN) identity->unified_id[pos++] = ':';
            size_t al = strlen(account_id);
            if (pos + al < SC_IDENTITY_UNIFIED_LEN) {
                size_t rem = SC_IDENTITY_UNIFIED_LEN - pos - 1;
                if (al > rem) al = rem;
                memcpy(identity->unified_id + pos, account_id, al);
                pos += al;
            }
        }
        if (pos + 1 < SC_IDENTITY_UNIFIED_LEN) identity->unified_id[pos++] = ':';
        size_t ul = strlen(user_id);
        size_t rem = SC_IDENTITY_UNIFIED_LEN - pos - 1;
        if (ul > rem) ul = rem;
        memcpy(identity->unified_id + pos, user_id, ul);
        identity->unified_id[pos + ul] = '\0';
    }
}

sc_permission_level_t sc_identity_resolve_level(const char *unified_id,
                                                const char *const *allowlist,
                                                size_t allowlist_count) {
    if (!unified_id) return SC_PERM_USER;
    if (allowlist) {
        for (size_t i = 0; i < allowlist_count; i++) {
            if (allowlist[i] && sc_util_strcasecmp(unified_id, allowlist[i]) == 0)
                return SC_PERM_ADMIN;  /* allowlist = admin */
        }
    }
    return SC_PERM_USER;
}

bool sc_identity_has_permission(sc_permission_level_t have,
                                sc_permission_level_t required) {
    return (int)have >= (int)required;
}
