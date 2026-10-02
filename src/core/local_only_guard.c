#include "human/core/local_only_guard.h"
#include "human/core/log.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* -1 = never configured (OFF). Otherwise a hu_gate_mode_t. */
static atomic_int g_configured = -1;
static atomic_uint_fast64_t g_refused = 0;
static atomic_uint_fast64_t g_audited = 0;
static _Thread_local const char *t_caller = NULL;

/* ── Locality: by endpoint, never by provider name ─────────────────────── */

static bool has_prefix_ci(const char *s, size_t len, const char *p) {
    size_t pl = strlen(p);
    return len >= pl && strncasecmp(s, p, pl) == 0;
}

static bool host_eq_ci(const char *h, size_t hl, const char *want) {
    return hl == strlen(want) && strncasecmp(h, want, hl) == 0;
}

/* Dotted quad 127.a.b.c with every octet 0..255. */
static bool host_is_ipv4_loopback(const char *h, size_t hl) {
    if (hl < 9 || memcmp(h, "127.", 4) != 0)
        return false;
    int octets = 0, val = -1;
    for (size_t i = 0; i <= hl; i++) {
        if (i == hl || h[i] == '.') {
            if (val < 0 || val > 255)
                return false;
            octets++;
            val = -1;
            continue;
        }
        if (h[i] < '0' || h[i] > '9')
            return false;
        val = (val < 0 ? 0 : val * 10) + (h[i] - '0');
        if (val > 255)
            return false;
    }
    return octets == 4;
}

static bool host_is_loopback(const char *h, size_t hl) {
    if (hl == 0)
        return false;
    if (host_eq_ci(h, hl, "localhost") || host_eq_ci(h, hl, "[::1]") ||
        host_eq_ci(h, hl, "[0:0:0:0:0:0:0:1]"))
        return true;
    if (hl > 10 && strncasecmp(h + hl - 10, ".localhost", 10) == 0)
        return true;
    return host_is_ipv4_loopback(h, hl);
}

/* Host span of a "scheme://[userinfo@]host[:port][/path]" URL. */
static bool url_host(const char *url, size_t len, const char **host, size_t *host_len) {
    const char *sep = NULL;
    for (size_t i = 0; i + 2 < len; i++) {
        if (url[i] == ':' && url[i + 1] == '/' && url[i + 2] == '/') {
            sep = url + i + 3;
            break;
        }
        if (url[i] == '/' || url[i] == '?' || url[i] == '#')
            return false; /* path before any scheme separator */
    }
    if (!sep)
        return false;
    const char *end = url + len;
    const char *auth_end = sep;
    while (auth_end < end && *auth_end != '/' && *auth_end != '?' && *auth_end != '#')
        auth_end++;
    const char *h = sep;
    for (const char *p = sep; p < auth_end; p++)
        if (*p == '@')
            h = p + 1; /* drop userinfo */
    const char *he = h;
    if (he < auth_end && *he == '[') {
        while (he < auth_end && *he != ']')
            he++;
        if (he == auth_end)
            return false;
        he++; /* keep the brackets */
    } else {
        while (he < auth_end && *he != ':')
            he++;
    }
    *host = h;
    *host_len = (size_t)(he - h);
    return *host_len > 0;
}

bool hu_provider_endpoint_is_local(const char *url, size_t url_len) {
    if (!url || url_len == 0)
        return false;
    if (has_prefix_ci(url, url_len, "unix:") || has_prefix_ci(url, url_len, "http+unix://"))
        return true;
    if (url[0] == '/')
        return true; /* absolute path: an in-process model file or a socket */
    if (!(has_prefix_ci(url, url_len, "http://") || has_prefix_ci(url, url_len, "https://") ||
          has_prefix_ci(url, url_len, "ws://") || has_prefix_ci(url, url_len, "wss://")))
        return false;
    const char *h = NULL;
    size_t hl = 0;
    return url_host(url, url_len, &h, &hl) && host_is_loopback(h, hl);
}

/* ── Which requests are model requests ─────────────────────────────────── */

/* Path suffixes (matched at the end of the path, query stripped). Specific
 * enough that channel APIs ("/channels/1/messages", "sendMessage") never hit. */
static const char *const k_model_path_suffixes[] = {
    "/chat/completions",     "/completions",        "/embeddings",
    "/v1/responses",         "/v1/messages",        "/api/chat",
    "/api/generate",         "/api/embed",          "/api/embeddings",
    "/audio/transcriptions", "/audio/translations",
};

/* Gemini / Vertex method suffixes ("models/<m>:generateContent"). */
static const char *const k_model_methods[] = {
    ":generateContent",    ":streamGenerateContent", ":predict",    ":embedContent",
    ":batchEmbedContents", ":countTokens",           ":rawPredict", ":streamRawPredict",
};

bool hu_local_only_url_is_model_request(const char *url) {
    if (!url || !url[0])
        return false;
    size_t len = strcspn(url, "?#");
    for (size_t i = 0; i < sizeof(k_model_methods) / sizeof(k_model_methods[0]); i++) {
        size_t ml = strlen(k_model_methods[i]);
        if (len >= ml && memcmp(url + len - ml, k_model_methods[i], ml) == 0)
            return true;
    }
    while (len > 0 && url[len - 1] == '/')
        len--;
    for (size_t i = 0; i < sizeof(k_model_path_suffixes) / sizeof(k_model_path_suffixes[0]); i++) {
        size_t sl = strlen(k_model_path_suffixes[i]);
        if (len >= sl && memcmp(url + len - sl, k_model_path_suffixes[i], sl) == 0)
            return true;
    }
    return false;
}

/* ── Mode ──────────────────────────────────────────────────────────────── */

int hu_local_only_env_parse(const char *v) {
    if (!v || !v[0])
        return -1;
    if (!strcasecmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "false"))
        return (int)HU_GATE_OFF;
    if (!strcasecmp(v, "audit") || !strcasecmp(v, "shadow"))
        return (int)HU_GATE_SHADOW;
    if (!strcasecmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "live") ||
        !strcasecmp(v, "true"))
        return (int)HU_GATE_LIVE;
    return -1;
}

hu_gate_mode_t hu_local_only_resolve(const char *env_value, int cfg_value, bool primary_is_local) {
    int env = hu_local_only_env_parse(env_value);
    if (env >= 0)
        return (hu_gate_mode_t)env;
    if (cfg_value == 0)
        return HU_GATE_OFF;
    if (cfg_value > 0)
        return HU_GATE_LIVE;
    return primary_is_local ? HU_GATE_LIVE : HU_GATE_OFF;
}

void hu_local_only_configure(hu_gate_mode_t mode) {
    atomic_store(&g_configured, (int)mode);
}

void hu_local_only_reset(void) {
    atomic_store(&g_configured, -1);
    atomic_store(&g_refused, 0);
    atomic_store(&g_audited, 0);
}

hu_gate_mode_t hu_local_only_mode(void) {
    int env = hu_local_only_env_parse(getenv("HU_LOCAL_ONLY"));
    if (env >= 0)
        return (hu_gate_mode_t)env;
    int cfg = atomic_load(&g_configured);
    return cfg < 0 ? HU_GATE_OFF : (hu_gate_mode_t)cfg;
}

bool hu_local_only_enforced(void) {
    return hu_local_only_mode() == HU_GATE_LIVE;
}

const char *hu_local_only_set_caller(const char *tag) {
    const char *prev = t_caller;
    t_caller = tag;
    return prev;
}

/* ── The check ─────────────────────────────────────────────────────────── */

/* Copy an identifier, keeping only [A-Za-z0-9._:-/]; never content. */
static void copy_ident(char *out, size_t cap, const char *s, size_t n) {
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < cap; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == ':' || c == '/';
        if (!ok)
            break;
        out[w++] = c;
    }
    out[w] = '\0';
    if (w == 0 && cap > 1)
        memcpy(out, "?", 2);
}

static void model_for_log(const char *url, const char *body, size_t body_len, char *out,
                          size_t cap) {
    const char *m = strstr(url, "/models/");
    if (m) {
        m += 8;
        copy_ident(out, cap, m, strcspn(m, ":?#/"));
        return;
    }
    static const char key[] = "\"model\":\"";
    if (body && body_len > sizeof(key)) {
        size_t limit = body_len < 4096 ? body_len : 4096; /* the field leads the body */
        for (size_t i = 0; i + sizeof(key) - 1 < limit; i++) {
            if (memcmp(body + i, key, sizeof(key) - 1) == 0) {
                const char *v = body + i + sizeof(key) - 1;
                size_t vn = 0;
                while (i + sizeof(key) - 1 + vn < body_len && v[vn] != '"')
                    vn++;
                copy_ident(out, cap, v, vn);
                return;
            }
        }
    }
    copy_ident(out, cap, "", 0);
}

hu_error_t hu_local_only_check_request(const char *url, const char *body, size_t body_len) {
    hu_gate_mode_t mode = hu_local_only_mode();
    if (mode == HU_GATE_OFF || !url)
        return HU_OK;
    if (!hu_local_only_url_is_model_request(url) || hu_provider_endpoint_is_local(url, strlen(url)))
        return HU_OK;

    char host[96], model[64];
    const char *h = NULL;
    size_t hl = 0;
    if (url_host(url, strlen(url), &h, &hl))
        copy_ident(host, sizeof(host), h, hl);
    else
        copy_ident(host, sizeof(host), "", 0);
    model_for_log(url, body, body_len, model, sizeof(model));
    const char *caller = t_caller ? t_caller : "unknown";

    if (mode == HU_GATE_SHADOW) {
        atomic_fetch_add(&g_audited, 1);
        hu_log_warn("local_only", NULL,
                    "[local_only audit] would refuse provider=%s model=%s caller=%s", host, model,
                    caller);
        return HU_OK;
    }
    atomic_fetch_add(&g_refused, 1);
    hu_log_warn("local_only", NULL, "[local_only] refused provider=%s model=%s caller=%s", host,
                model, caller);
    return HU_ERR_PERMISSION_DENIED;
}

uint64_t hu_local_only_refused_count(void) {
    return (uint64_t)atomic_load(&g_refused);
}

uint64_t hu_local_only_audit_count(void) {
    return (uint64_t)atomic_load(&g_audited);
}
