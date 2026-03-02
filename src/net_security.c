#include "seaclaw/net_security.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <ctype.h>

static int parse_ipv4(const char *s, unsigned char octets[4])
{
    int count = 0;
    const char *start = s;

    for (;;) {
        if (*s == '.' || *s == '\0') {
            if (s == start) return -1;
            unsigned v = 0;
            for (const char *p = start; p < s; p++) {
                if (*p < '0' || *p > '9') return -1;
                v = v * 10 + (*p - '0');
            }
            if (v > 255) return -1;
            octets[count++] = (unsigned char)v;
            if (count == 4) break;
            if (*s == '\0') break;
            start = s + 1;
            s++;
        } else {
            s++;
        }
    }
    return (count == 4) ? 0 : -1;
}

static int parse_ipv6_quad(const char *s, const char *end, unsigned short *out)
{
    if (s >= end) return -1;
    unsigned v = 0;
    int digits = 0;
    while (s < end && ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) {
        if (*s >= '0' && *s <= '9') v = v * 16 + (*s - '0');
        else if (*s >= 'a' && *s <= 'f') v = v * 16 + (*s - 'a' + 10);
        else v = v * 16 + (*s - 'A' + 10);
        s++;
        digits++;
    }
    if (digits == 0 || digits > 4 || v > 0xFFFF) return -1;
    *out = (unsigned short)v;
    return 0;
}

static int parse_ipv6(const char *s, unsigned short segs[8])
{
    /* Simplified: handle ::1 and fe80::1 style. Full IPv6 is complex. */
    memset(segs, 0, sizeof(segs[0]) * 8);
    size_t len = strlen(s);
    if (len == 0) return -1;

    /* Strip brackets if present */
    if (s[0] == '[') {
        if (len < 2 || s[len - 1] != ']') return -1;
        s++;
        len -= 2;
    }

    /* Strip zone id (e.g. %lo0) */
    const char *pct = memchr(s, '%', len);
    if (pct) len = (size_t)(pct - s);

    if (len == 0) return -1;

    /* ::1 */
    if (strncmp(s, "::1", len) == 0 && len == 3) {
        segs[7] = 1;
        return 0;
    }
    /* :: */
    if (strncmp(s, "::", len) == 0 && len == 2) {
        return 0;
    }
    /* fe80::1, fd00::1, etc. */
    const char *dbl = strstr(s, "::");
    if (dbl) {
        /* Parse before :: */
        const char *head = s;
        const char *head_end = dbl;
        int idx = 0;
        while (head < head_end && idx < 8) {
            while (head < head_end && *head == ':') head++;
            if (head >= head_end) break;
            const char *seg_start = head;
            while (head < head_end && *head != ':') head++;
            unsigned short v;
            if (parse_ipv6_quad(seg_start, head, &v) != 0) return -1;
            segs[idx++] = v;
        }
        /* Parse after :: */
        const char *tail = dbl + 2;
        while (*tail == ':') tail++;
        if (*tail) {
            /* Tail segments go at end */
            int tail_count = 0;
            unsigned short tail_segs[8];
            const char *t = tail;
            while (*t && tail_count < 8) {
                while (*t == ':') t++;
                if (!*t) break;
                const char *ts = t;
                while (*t && *t != ':') t++;
                if (parse_ipv6_quad(ts, t, &tail_segs[tail_count]) != 0) return -1;
                tail_count++;
            }
            int gap = 8 - idx - tail_count;
            for (int i = 0; i < tail_count; i++)
                segs[idx + gap + i] = tail_segs[i];
        }
        return 0;
    }

    /* No :: - parse 8 groups */
    const char *p = s;
    int idx = 0;
    while (*p && idx < 8) {
        while (*p == ':') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ':') p++;
        if (parse_ipv6_quad(start, p, &segs[idx]) != 0) return -1;
        idx++;
    }
    return (idx == 8) ? 0 : -1;
}

static int is_non_global_v4(const unsigned char a[4])
{
    if (a[0] == 127) return 1;  /* loopback */
    if (a[0] == 10) return 1;    /* 10.0.0.0/8 */
    if (a[0] == 172 && a[1] >= 16 && a[1] <= 31) return 1;  /* 172.16-31 */
    if (a[0] == 192 && a[1] == 168) return 1;  /* 192.168.0.0/16 */
    if (a[0] == 0) return 1;    /* unspecified */
    if (a[0] == 169 && a[1] == 254) return 1;  /* link-local */
    if (a[0] >= 224) return 1;  /* multicast/broadcast */
    if (a[0] == 100 && a[1] >= 64 && a[1] <= 127) return 1;  /* shared */
    if (a[0] == 192 && a[1] == 0 && a[2] == 2) return 1;  /* TEST-NET */
    if (a[0] == 198 && a[1] == 51 && a[2] == 100) return 1;
    if (a[0] == 203 && a[1] == 0 && a[2] == 113) return 1;
    if (a[0] == 198 && (a[1] == 18 || a[1] == 19)) return 1;  /* benchmark */
    if (a[0] == 192 && a[1] == 0 && a[2] == 0) return 1;
    return 0;
}

static int is_non_global_v6(const unsigned short segs[8])
{
    if (segs[0] == 0 && segs[1] == 0 && segs[2] == 0 && segs[3] == 0 &&
        segs[4] == 0 && segs[5] == 0 && segs[6] == 0 && segs[7] == 1)
        return 1;  /* ::1 */
    if (segs[0] == 0 && segs[1] == 0 && segs[2] == 0 && segs[3] == 0 &&
        segs[4] == 0 && segs[5] == 0 && segs[6] == 0 && segs[7] == 0)
        return 1;  /* :: */
    if ((segs[0] & 0xFF00) == 0xFF00) return 1;  /* multicast */
    if ((segs[0] & 0xFE00) == 0xFC00) return 1;  /* unique local */
    if ((segs[0] & 0xFFC0) == 0xFE80) return 1;  /* link-local */
    if (segs[0] == 0x2001 && segs[1] == 0x0DB8) return 1;  /* documentation */
    /* IPv4-mapped */
    if (segs[0] == 0 && segs[1] == 0 && segs[2] == 0 && segs[3] == 0 &&
        segs[4] == 0 && segs[5] == 0xFFFF) {
        unsigned char v4[4] = {
            (unsigned char)(segs[6] >> 8),
            (unsigned char)(segs[6] & 0xFF),
            (unsigned char)(segs[7] >> 8),
            (unsigned char)(segs[7] & 0xFF)
        };
        return is_non_global_v4(v4);
    }
    return 0;
}

static int ascii_ci_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return 1;
}

static int is_localhost_name(const char *host, size_t len)
{
    if (len == 9 && ascii_ci_eq(host, "localhost", 9)) return 1;
    if (len > 10 && host[len - 10] == '.' && ascii_ci_eq(host + len - 9, "localhost", 9))
        return 1;  /* *.localhost */
    if (len >= 6 && ascii_ci_eq(host + len - 6, ".local", 6))
        return 1;
    return 0;
}

static void extract_host_from_url(const char *url, const char **host_out, size_t *host_len_out)
{
    *host_out = NULL;
    *host_len_out = 0;

    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) return;

    const char *authority = scheme_end + 3;
    /* Skip userinfo if present (user:pass@host) */
    const char *at = strchr(authority, '@');
    if (at) authority = at + 1;

    const char *host_start = authority;
    const char *host_end;

    if (*authority == '[') {
        const char *rb = strchr(authority + 1, ']');
        if (!rb) return;
        host_start = authority;
        host_end = rb + 1;
    } else {
        const char *p = authority;
        while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ':')
            p++;
        host_end = p;
    }

    while (host_end > host_start && host_end[-1] == '.') host_end--;
    if (host_end > host_start)
        *host_len_out = (size_t)(host_end - host_start);
    *host_out = host_start;
}

static int is_https(const char *url)
{
    size_t n = strlen(url);
    if (n < 8) return 0;
    return (url[0] == 'h' || url[0] == 'H') &&
           (url[1] == 't' || url[1] == 'T') &&
           (url[2] == 't' || url[2] == 'T') &&
           (url[3] == 'p' || url[3] == 'P') &&
           (url[4] == 's' || url[4] == 'S') &&
           url[5] == ':' && url[6] == '/' && url[7] == '/';
}

static int is_http(const char *url)
{
    size_t n = strlen(url);
    if (n < 8) return 0;
    return (url[0] == 'h' || url[0] == 'H') &&
           (url[1] == 't' || url[1] == 'T') &&
           (url[2] == 't' || url[2] == 'T') &&
           (url[3] == 'p' || url[3] == 'P') &&
           url[4] == ':' && url[5] == '/' && url[6] == '/';
}

sc_error_t sc_validate_url(const char *url)
{
    if (!url || !url[0]) return SC_ERR_INVALID_ARGUMENT;

    if (is_https(url))
        return SC_OK;

    if (is_http(url)) {
        const char *host;
        size_t host_len;
        extract_host_from_url(url, &host, &host_len);
        if (!host || host_len == 0) return SC_ERR_INVALID_ARGUMENT;
        /* Allow http only for localhost */
        if (is_localhost_name(host, host_len)) return SC_OK;
        return SC_ERR_INVALID_ARGUMENT;  /* non-HTTPS to non-localhost */
    }

    return SC_ERR_INVALID_ARGUMENT;
}

bool sc_is_private_ip(const char *ip)
{
    if (!ip || !ip[0]) return true;  /* fail closed */

    /* Strip brackets for IPv6 */
    const char *bare = ip;
    size_t len = strlen(ip);
    if (len >= 2 && ip[0] == '[' && ip[len - 1] == ']') {
        bare = ip + 1;
        len -= 2;
    }

    /* Strip zone id */
    const char *pct = memchr(bare, '%', len);
    if (pct) len = (size_t)(pct - bare);
    if (len == 0) return true;

    /* localhost names */
    if (is_localhost_name(bare, len)) return true;

    /* IPv4 */
    unsigned char octets[4];
    char v4buf[64];
    if (len < sizeof(v4buf)) {
        memcpy(v4buf, bare, len);
        v4buf[len] = '\0';
        if (parse_ipv4(v4buf, octets) == 0)
            return is_non_global_v4(octets) != 0;
    }

    /* IPv6 */
    unsigned short segs[8];
    char v6buf[64];
    if (len < sizeof(v6buf)) {
        memcpy(v6buf, bare, len);
        v6buf[len] = '\0';
        if (parse_ipv6(v6buf, segs) == 0)
            return is_non_global_v6(segs) != 0;
    }

    return false;  /* unknown format - treat as public */
}

bool sc_validate_domain(const char *host,
    const char *const *allowed,
    size_t allowed_count)
{
    if (!host || !host[0]) return false;
    if (allowed_count == 0) return true;  /* empty allowlist = allow all */

    size_t host_len = strlen(host);

    for (size_t i = 0; i < allowed_count; i++) {
        const char *pattern = allowed[i];
        if (!pattern) continue;
        size_t plen = strlen(pattern);

        /* Exact match */
        if (plen == host_len && memcmp(host, pattern, plen) == 0)
            return true;

        /* Wildcard *.domain */
        if (plen >= 2 && pattern[0] == '*' && pattern[1] == '.') {
            const char *domain = pattern + 2;
            size_t dlen = plen - 2;
            if (host_len >= dlen + 2 &&
                memcmp(host + host_len - dlen, domain, dlen) == 0 &&
                host[host_len - dlen - 1] == '.')
                return true;
        }

        /* Implicit subdomain: api.example.com matches example.com */
        if (host_len > plen + 1 &&
            host[host_len - plen - 1] == '.' &&
            memcmp(host + host_len - plen, pattern, plen) == 0)
            return true;
    }
    return false;
}
