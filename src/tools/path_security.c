#include "seaclaw/tools/path_security.h"
#include "seaclaw/core/allocator.h"
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define SC_PATH_SEP '\\'
#define SC_ALT_SEP '/'
#else
#define SC_PATH_SEP '/'
#define SC_ALT_SEP '/'
#endif

static bool path_starts_with(const char *path, const char *prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) return false;
    if (path[plen] == '\0') return true;
    if (path[plen] == SC_PATH_SEP || path[plen] == SC_ALT_SEP) return true;
    return false;
}

bool sc_path_is_safe(const char *path) {
    if (!path) return false;
    if (path[0] == '/' || path[0] == '\\') return false;
    if (strlen(path) >= 3 && isalpha((unsigned char)path[0]) && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\\'))
        return false;

    const char *p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.') {
            bool sep_before = (p == path || p[-1] == '/' || p[-1] == '\\');
            bool sep_after = (p[2] == '/' || p[2] == '\\' || p[2] == '\0');
            if (sep_before && sep_after) return false;
        }
        p++;
    }

    /* URL-encoded traversal */
    char lower[4096];
    size_t len = strlen(path);
    if (len >= sizeof(lower)) return false;
    for (size_t i = 0; i < len; i++)
        lower[i] = (char)tolower((unsigned char)path[i]);
    lower[len] = '\0';
    if (strstr(lower, "..%2f") || strstr(lower, "%2f..") ||
        strstr(lower, "..%5c") || strstr(lower, "%5c.."))
        return false;

    return true;
}

static const char *const SYSTEM_BLOCKED[] = {
    "/System", "/Library", "/bin", "/sbin", "/usr/bin", "/usr/sbin",
    "/usr/lib", "/usr/libexec", "/etc", "/private/etc", "/private/var",
    "/dev", "/boot", "/proc", "/sys",
    NULL
};

#ifdef _WIN32
static const char *const SYSTEM_BLOCKED_WIN[] = {
    "C:\\Windows", "C:\\Program Files", "C:\\Program Files (x86)",
    "C:\\ProgramData", "C:\\System32", "C:\\Recovery",
    NULL
};
#endif

bool sc_path_resolved_allowed(sc_allocator_t *alloc,
    const char *resolved,
    const char *ws_resolved,
    const char *const *allowed_paths,
    size_t allowed_count)
{
    (void)alloc;
    if (!resolved) return false;

#ifdef _WIN32
    for (size_t i = 0; SYSTEM_BLOCKED_WIN[i]; i++) {
        if (path_starts_with(resolved, SYSTEM_BLOCKED_WIN[i]))
            return false;
    }
#else
    for (size_t i = 0; SYSTEM_BLOCKED[i]; i++) {
        if (path_starts_with(resolved, SYSTEM_BLOCKED[i]))
            return false;
    }
#endif

    if (ws_resolved && ws_resolved[0]) {
        if (path_starts_with(resolved, ws_resolved))
            return true;
    }

    for (size_t i = 0; i < allowed_count; i++) {
        const char *ap = allowed_paths[i];
        if (!ap) continue;
        while (*ap == ' ' || *ap == '\t') ap++;
        size_t aplen = strlen(ap);
        while (aplen > 0 && (ap[aplen - 1] == ' ' || ap[aplen - 1] == '\t')) aplen--;
        if (aplen == 0) continue;
        if (aplen == 1 && ap[0] == '*') return true;
        if (path_starts_with(resolved, ap)) return true;
    }

    return false;
}
