#include "seaclaw/security.h"
#include <string.h>

bool sc_security_path_allowed(const sc_security_policy_t *policy,
    const char *path, size_t path_len)
{
    if (!policy || !path) return false;
    if (!policy->allowed_paths || policy->allowed_paths_count == 0) return false;
    for (size_t i = 0; i < policy->allowed_paths_count; i++) {
        const char *p = policy->allowed_paths[i];
        if (!p) continue;
        size_t plen = strlen(p);
        if (path_len >= plen && memcmp(path, p, plen) == 0) {
            if (path_len == plen || path[plen] == '/' || path[plen] == '\0')
                return true;
        }
    }
    return false;
}

bool sc_security_shell_allowed(const sc_security_policy_t *policy) {
    if (!policy) return false;
    return policy->allow_shell;
}
