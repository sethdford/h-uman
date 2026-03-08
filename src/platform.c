#include "seaclaw/platform.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__)
#include <windows.h>
#define SC_IS_WIN 1
#if defined(_WIN32) && !defined(__CYGWIN__)
#include <direct.h>
#include <stdio.h>
#endif
#else
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#define SC_IS_WIN 0
#endif

char *sc_platform_get_env(sc_allocator_t *alloc, const char *name) {
    if (!alloc || !name)
        return NULL;
#if SC_IS_WIN
    (void)alloc;
    /* Windows getenv returns pointer to process env block; dupe for ownership */
    const char *v = getenv(name);
    if (!v)
        return NULL;
    size_t len = strlen(v) + 1;
    char *out = alloc->alloc(alloc->ctx, len);
    if (!out)
        return NULL;
    memcpy(out, v, len);
    return out;
#else
    const char *v = getenv(name);
    if (!v)
        return NULL;
    size_t len = strlen(v) + 1;
    char *out = alloc->alloc(alloc->ctx, len);
    if (!out)
        return NULL;
    memcpy(out, v, len);
    return out;
#endif
}

char *sc_platform_get_home_dir(sc_allocator_t *alloc) {
    if (!alloc)
        return NULL;
#if SC_IS_WIN
    char *prof = sc_platform_get_env(alloc, "USERPROFILE");
    if (prof)
        return prof;
    char *drive = sc_platform_get_env(alloc, "HOMEDRIVE");
    if (!drive)
        return NULL;
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
    if (!alloc)
        return NULL;
#if SC_IS_WIN
    char *v = sc_platform_get_env(alloc, "TEMP");
    if (v)
        return v;
    v = sc_platform_get_env(alloc, "TMP");
    if (v)
        return v;
    {
        const char *def = "C:\\Temp";
        size_t len = strlen(def) + 1;
        char *out = alloc->alloc(alloc->ctx, len);
        if (out)
            memcpy(out, def, len);
        return out;
    }
#else
    char *v = sc_platform_get_env(alloc, "TMPDIR");
    if (v)
        return v;
    {
        const char *def = "/tmp";
        size_t len = strlen(def) + 1;
        char *out = alloc->alloc(alloc->ctx, len);
        if (out)
            memcpy(out, def, len);
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

bool sc_platform_is_windows(void) {
    return SC_IS_WIN ? true : false;
}
bool sc_platform_is_unix(void) {
    return SC_IS_WIN ? false : true;
}

/* ─── Cross-platform time, sleep, filesystem ─────────────────────────────── */

struct tm *sc_platform_localtime_r(const time_t *t, struct tm *out) {
    if (!t || !out)
        return NULL;
#if defined(_WIN32) && !defined(__CYGWIN__)
    return (localtime_s(out, t) == 0) ? out : NULL;
#else
    return localtime_r(t, out);
#endif
}

struct tm *sc_platform_gmtime_r(const time_t *t, struct tm *out) {
    if (!t || !out)
        return NULL;
#if defined(_WIN32) && !defined(__CYGWIN__)
    return (gmtime_s(out, t) == 0) ? out : NULL;
#else
    return gmtime_r(t, out);
#endif
}

void sc_platform_sleep_ms(unsigned int ms) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    Sleep(ms);
#else
    if (ms >= 1000) {
        unsigned int sec = ms / 1000;
        sleep(sec);
        ms -= sec * 1000;
    }
    if (ms > 0) {
        struct timespec ts = {.tv_sec = (time_t)(ms / 1000), .tv_nsec = (long)((ms % 1000) * 1000000)};
        nanosleep(&ts, NULL);
    }
#endif
}

int sc_platform_mkdir(const char *path, unsigned int mode) {
    (void)mode;
#if defined(_WIN32) && !defined(__CYGWIN__)
    return _mkdir(path);
#else
    return mkdir(path, (mode_t)mode);
#endif
}

char *sc_platform_realpath(sc_allocator_t *alloc, const char *path) {
    if (!alloc || !path)
        return NULL;
#if defined(_WIN32) && !defined(__CYGWIN__)
    char *resolved = (char *)alloc->alloc(alloc->ctx, _MAX_PATH);
    if (!resolved)
        return NULL;
    if (!_fullpath(resolved, path, _MAX_PATH)) {
        alloc->free(alloc->ctx, resolved, _MAX_PATH);
        return NULL;
    }
    size_t len = strlen(resolved) + 1;
    char *out = (char *)alloc->alloc(alloc->ctx, len);
    if (!out) {
        alloc->free(alloc->ctx, resolved, _MAX_PATH);
        return NULL;
    }
    memcpy(out, resolved, len);
    alloc->free(alloc->ctx, resolved, _MAX_PATH);
    return out;
#else
    char *r = realpath(path, NULL);
    if (!r)
        return NULL;
    size_t len = strlen(r) + 1;
    char *out = (char *)alloc->alloc(alloc->ctx, len);
    if (!out) {
        free(r);
        return NULL;
    }
    memcpy(out, r, len);
    free(r);
    return out;
#endif
}

bool sc_platform_parse_datetime(const char *ts, struct tm *out) {
    if (!ts || !ts[0] || !out)
        return false;
    memset(out, 0, sizeof(*out));
#if defined(_WIN32) && !defined(__CYGWIN__)
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (sscanf(ts, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
        out->tm_year = y - 1900;
        out->tm_mon = mo - 1;
        out->tm_mday = d;
        out->tm_hour = h;
        out->tm_min = mi;
        out->tm_isdst = -1;
        return true;
    }
    if (sscanf(ts, "%d:%d", &h, &mi) == 2) {
        time_t now = time(NULL);
        struct tm now_tm;
        if (sc_platform_localtime_r(&now, &now_tm)) {
            out->tm_year = now_tm.tm_year;
            out->tm_mon = now_tm.tm_mon;
            out->tm_mday = now_tm.tm_mday;
        }
        out->tm_hour = h;
        out->tm_min = mi;
        out->tm_isdst = -1;
        return true;
    }
    return false;
#else
    char *p = strptime(ts, "%Y-%m-%d %H:%M", out);
    if (p && *p == '\0')
        return true;
    memset(out, 0, sizeof(*out));
    time_t now = time(NULL);
    struct tm now_tm;
    if (!sc_platform_localtime_r(&now, &now_tm))
        return false;
    out->tm_year = now_tm.tm_year;
    out->tm_mon = now_tm.tm_mon;
    out->tm_mday = now_tm.tm_mday;
    p = strptime(ts, "%H:%M", out);
    return (p && *p == '\0');
#endif
}

const char *sc_platform_get_home_env(void) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    return getenv("USERPROFILE") ? getenv("USERPROFILE") : getenv("HOME");
#else
    return getenv("HOME");
#endif
}
