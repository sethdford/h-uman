#include "seaclaw/platform.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__)
#include <windows.h>
#define SC_IS_WIN 1
#else
#include <unistd.h>
#include <errno.h>
#define SC_IS_WIN 0
#endif

char *sc_platform_get_env(sc_allocator_t *alloc, const char *name) {
    if (!alloc || !name) return NULL;
#if SC_IS_WIN
    (void)alloc;
    /* Windows getenv returns pointer to process env block; dupe for ownership */
    const char *v = getenv(name);
    if (!v) return NULL;
    size_t len = strlen(v) + 1;
    char *out = alloc->alloc(alloc->ctx, len);
    if (!out) return NULL;
    memcpy(out, v, len);
    return out;
#else
    const char *v = getenv(name);
    if (!v) return NULL;
    size_t len = strlen(v) + 1;
    char *out = alloc->alloc(alloc->ctx, len);
    if (!out) return NULL;
    memcpy(out, v, len);
    return out;
#endif
}

char *sc_platform_get_home_dir(sc_allocator_t *alloc) {
    if (!alloc) return NULL;
#if SC_IS_WIN
    char *prof = sc_platform_get_env(alloc, "USERPROFILE");
    if (prof) return prof;
    char *drive = sc_platform_get_env(alloc, "HOMEDRIVE");
    if (!drive) return NULL;
    char *path = sc_platform_get_env(alloc, "HOMEPATH");
    if (!path) {
        alloc->free(alloc->ctx, drive, strlen(drive) + 1);
        return NULL;
    }
    size_t dlen = strlen(drive), plen = strlen(path);
    char *out = alloc->alloc(alloc->ctx, dlen + plen + 1);
    if (!out) {
        alloc->free(alloc->ctx, drive, strlen(drive) + 1);
        alloc->free(alloc->ctx, path, strlen(path) + 1);
        return NULL;
    }
    memcpy(out, drive, dlen);
    memcpy(out + dlen, path, plen + 1);
    alloc->free(alloc->ctx, drive, strlen(drive) + 1);
    alloc->free(alloc->ctx, path, strlen(path) + 1);
    return out;
#else
    return sc_platform_get_env(alloc, "HOME");
#endif
}

char *sc_platform_get_temp_dir(sc_allocator_t *alloc) {
    if (!alloc) return NULL;
#if SC_IS_WIN
    char *v = sc_platform_get_env(alloc, "TEMP");
    if (v) return v;
    v = sc_platform_get_env(alloc, "TMP");
    if (v) return v;
    {
        const char *def = "C:\\Temp";
        size_t len = strlen(def) + 1;
        char *out = alloc->alloc(alloc->ctx, len);
        if (out) memcpy(out, def, len);
        return out;
    }
#else
    char *v = sc_platform_get_env(alloc, "TMPDIR");
    if (v) return v;
    {
        const char *def = "/tmp";
        size_t len = strlen(def) + 1;
        char *out = alloc->alloc(alloc->ctx, len);
        if (out) memcpy(out, def, len);
        return out;
    }
#endif
}

const char *sc_platform_get_shell(void) {
#if SC_IS_WIN
    return "cmd.exe";
#else
    return "/bin/sh";
#endif
}

const char *sc_platform_get_shell_flag(void) {
#if SC_IS_WIN
    return "/c";
#else
    return "-c";
#endif
}

bool sc_platform_is_windows(void) { return SC_IS_WIN ? true : false; }
bool sc_platform_is_unix(void) { return SC_IS_WIN ? false : true; }
