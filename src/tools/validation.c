/*
 * Tool validation helpers: path and URL security checks.
 * Secure by default - deny unless explicitly allowed.
 */
#include "seaclaw/tools/validation.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#define SC_PATH_MAX 4096
#define SC_URL_MAX  8192

sc_error_t sc_tool_validate_path(const char *path,
    const char *workspace_dir, size_t workspace_dir_len)
{
    if (!path || path[0] == '\0')
        return SC_ERR_TOOL_VALIDATION;

    size_t len = strlen(path);
    if (len > SC_PATH_MAX)
        return SC_ERR_TOOL_VALIDATION;

    const char *p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.') {
            bool sep_before = (p == path || p[-1] == '/' || p[-1] == '\\');
            bool sep_after = (p[2] == '/' || p[2] == '\\' || p[2] == '\0');
            if (sep_before && sep_after)
                return SC_ERR_TOOL_VALIDATION;
        }
        p++;
    }

    /* If workspace_dir set, reject absolute paths outside workspace */
    if (workspace_dir && workspace_dir_len > 0) {
        bool is_absolute = (path[0] == '/') ||
            (len >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
             path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
        if (is_absolute) {
            /* Absolute path: must be under workspace_dir */
            size_t ws_len = workspace_dir_len;
            while (ws_len > 0 && workspace_dir[ws_len - 1] == '/')
                ws_len--;
            if (len < ws_len)
                return SC_ERR_TOOL_VALIDATION;
            if (memcmp(path, workspace_dir, ws_len) != 0)
                return SC_ERR_TOOL_VALIDATION;
            if (path[ws_len] != '\0' && path[ws_len] != '/' && path[ws_len] != '\\')
                return SC_ERR_TOOL_VALIDATION;
        }
    }

    return SC_OK;
}

static bool parse_ipv4_private(const char *host, size_t len) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    size_t i = 0;
    if (len == 0) return true;
    while (i < len && host[i] >= '0' && host[i] <= '9') { a = a * 10 + (unsigned)(host[i] - '0'); i++; }
    if (i >= len || host[i] != '.') return true;
    i++;
    while (i < len && host[i] >= '0' && host[i] <= '9') { b = b * 10 + (unsigned)(host[i] - '0'); i++; }
    if (i >= len || host[i] != '.') return true;
    i++;
    while (i < len && host[i] >= '0' && host[i] <= '9') { c = c * 10 + (unsigned)(host[i] - '0'); i++; }
    if (i >= len || host[i] != '.') return true;
    i++;
    while (i < len && host[i] >= '0' && host[i] <= '9') { d = d * 10 + (unsigned)(host[i] - '0'); i++; }
    if (i != len) return true;
    /* 127.x.x.x */
    if (a == 127) return true;
    /* 10.x.x.x */
    if (a == 10) return true;
    /* 172.16-31.x.x */
    if (a == 172 && b >= 16 && b <= 31) return true;
    /* 192.168.x.x */
    if (a == 192 && b == 168) return true;
    return false;
}

static bool parse_ipv6_private(const char *host, size_t len) {
    if (len < 2) return true;
    /* ::1 */
    if ((len == 3 || len == 4) && host[0] == ':' && host[1] == ':') {
        if (len == 3 && host[2] == '1') return true;
        if (len == 4 && host[2] == '1' && (host[3] == '\0' || host[3] == ']')) return true;
    }
    /* fdxx: or fe80: (link-local) */
    if (len >= 2) {
        char c0 = (char)tolower((unsigned char)host[0]);
        char c1 = (char)tolower((unsigned char)host[1]);
        if (c0 == 'f' && c1 == 'd') return true;  /* fd00::/8 */
        if (c0 == 'f' && c1 == 'e') return true;  /* fe80::/10 */
    }
    return false;
}

/* Extract host from https://host:port/path - returns pointer and length */
static void extract_host(const char *url, size_t url_len, const char **out_host, size_t *out_len) {
    *out_host = NULL;
    *out_len = 0;
    if (url_len < 8) return;
    const char *p = url + 8;  /* skip "https://" */
    const char *end = url + url_len;
    const char *start = p;
    while (p < end && *p && *p != '/' && *p != '?' && *p != '#') {
        if (*p == '[') {
            /* IPv6 literal */
            const char *bracket = p;
            p++;
            while (p < end && *p && *p != ']') p++;
            if (p < end && *p == ']') {
                *out_host = bracket + 1;
                *out_len = (size_t)(p - bracket - 1);
                return;
            }
            return;
        }
        if (*p == ':') {
            /* port follows */
            *out_host = start;
            *out_len = (size_t)(p - start);
            return;
        }
        p++;
    }
    *out_host = start;
    *out_len = (size_t)(p - start);
}

sc_error_t sc_tool_validate_url(const char *url)
{
    if (!url || url[0] == '\0')
        return SC_ERR_TOOL_VALIDATION;

    size_t len = strlen(url);
    if (len > SC_URL_MAX)
        return SC_ERR_TOOL_VALIDATION;

    /* HTTPS only */
    if (len < 8)
        return SC_ERR_TOOL_VALIDATION;
    if (tolower((unsigned char)url[0]) != 'h' || tolower((unsigned char)url[1]) != 't' ||
        tolower((unsigned char)url[2]) != 't' || tolower((unsigned char)url[3]) != 'p' ||
        url[4] != 's' || url[5] != ':' || url[6] != '/' || url[7] != '/')
        return SC_ERR_TOOL_VALIDATION;

    const char *host;
    size_t host_len;
    extract_host(url, len, &host, &host_len);
    if (!host || host_len == 0)
        return SC_ERR_TOOL_VALIDATION;

    /* Check for IPv4 (all digits and dots) */
    size_t i;
    bool might_ipv4 = true;
    for (i = 0; i < host_len && might_ipv4; i++) {
        char c = host[i];
        if (c >= '0' && c <= '9') continue;
        if (c == '.') continue;
        might_ipv4 = false;
    }
    if (might_ipv4 && host_len <= 15) {
        if (parse_ipv4_private(host, host_len))
            return SC_ERR_TOOL_VALIDATION;
        return SC_OK;
    }

    /* Check for IPv6 (host was extracted from [::1] so no brackets in host) */
    if (host_len > 2) {
        bool has_colon = false;
        for (i = 0; i < host_len; i++) if (host[i] == ':') { has_colon = true; break; }
        if (has_colon && parse_ipv6_private(host, host_len))
            return SC_ERR_TOOL_VALIDATION;
    }

    /* Hostname - allow (no private IP) */
    return SC_OK;
}
