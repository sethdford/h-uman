#include "human/security.h"
#include <string.h>

bool hu_security_path_allowed(const hu_security_policy_t *policy, const char *path,
                              size_t path_len) {
    if (!policy || !path)
        return false;

    /* When allowed_paths is empty, allow paths under workspace_dir (workspace-only mode) */
    if (!policy->allowed_paths || policy->allowed_paths_count == 0) {
        if (policy->workspace_dir && policy->workspace_dir[0]) {
            size_t wlen = strlen(policy->workspace_dir);
            while (wlen > 0 && policy->workspace_dir[wlen - 1] == '/')
                wlen--;
            if (wlen > 0 && path_len >= wlen && memcmp(path, policy->workspace_dir, wlen) == 0) {
                if (path_len == wlen || path[wlen] == '/' || path[wlen] == '\0')
                    return true;
            }
        }
        return false;
    }

    for (size_t i = 0; i < policy->allowed_paths_count; i++) {
        const char *p = policy->allowed_paths[i];
        if (!p)
            continue;
        size_t plen = strlen(p);
        if (path_len >= plen && memcmp(path, p, plen) == 0) {
            if (path_len == plen || path[plen] == '/' || path[plen] == '\0')
                return true;
        }
    }
    return false;
}

bool hu_security_shell_allowed(const hu_security_policy_t *policy) {
    if (!policy)
        return false;
    return policy->allow_shell;
}
