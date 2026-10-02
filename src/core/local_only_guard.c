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
    "/chat/completions", "/completions",        "/embeddings",   "/v1/responses",
    "/v1/messages",      "/api/chat",           "/api/generate", "/api/embed",
    "/api/embeddings",   "/images/generations", "/images/edits", "/images/variations",
    "/v1/realtime",
};

/* Gemini / Vertex method suffixes ("models/<m>:generateContent"). */
static const char *const k_model_methods[] = {
    ":generateContent", ":streamGenerateContent", ":predict",
    ":embedContent",    ":batchEmbedContents",    ":countTokens",
    ":rawPredict",      ":streamRawPredict",      ":predictLongRunning",
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

/* ── Cloud model names and provider-level locality (shared with #581) ──── */

static const char *const k_cloud_model_prefixes[] = {
    "gemini", "gpt-", "chatgpt", "claude", "grok",
};

bool hu_local_only_model_name_is_cloud(const char *model, size_t model_len) {
    if (!model || model_len == 0)
        return false;
    for (size_t i = model_len; i > 0; i--) { /* "publishers/google/models/gemini-..." */
        if (model[i - 1] == '/') {
            model += i;
            model_len -= i;
            break;
        }
    }
    for (size_t i = 0; i < sizeof(k_cloud_model_prefixes) / sizeof(k_cloud_model_prefixes[0]);
         i++) {
        size_t pl = strlen(k_cloud_model_prefixes[i]);
        if (model_len >= pl && strncasecmp(model, k_cloud_model_prefixes[i], pl) == 0)
            return true;
    }
    /* OpenAI o-series: "o1", "o3-pro", "o4-mini" (o + digits, then end or '-'). */
    if (model_len >= 2 && (model[0] == 'o' || model[0] == 'O') && model[1] >= '0' &&
        model[1] <= '9') {
        size_t j = 1;
        while (j < model_len && model[j] >= '0' && model[j] <= '9')
            j++;
        if (j == model_len || model[j] == '-')
            return true;
    }
    /* Ollama cloud models run on ollama.com: "gpt-oss:120b-cloud", "x:cloud". */
    static const char k_cloud_suffix[] = "-cloud";
    size_t sl = sizeof(k_cloud_suffix) - 1;
    if (model_len >= sl && strncasecmp(model + model_len - sl, k_cloud_suffix, sl) == 0)
        return true;
    for (size_t i = 0; i + 6 <= model_len; i++) {
        if (model[i] == ':' && strncasecmp(model + i + 1, "cloud", 5) == 0 &&
            (i + 6 == model_len || model[i + 6] == '-' || model[i + 6] == ':'))
            return true;
    }
    return false;
}

/* Gateways that forward to cloud APIs even when they listen on loopback. */
static const char *const k_cloud_proxy_names[] = {
    "openrouter", "litellm", "portkey", "helicone", "together", "groq", "fireworks", "requesty",
};

/* In-process backends: local when no URL is configured. */
static const char *const k_in_process_names[] = {
    "apple", "apfel",    "apple-intelligence", "foundationmodels", "coreml",
    "mlx",   "embedded", "llama-cli",          "llamacpp",         "huml",
};

static bool name_in(const char *name, const char *const *list, size_t n) {
    if (!name || !name[0])
        return false;
    for (size_t i = 0; i < n; i++)
        if (strcasecmp(name, list[i]) == 0)
            return true;
    return false;
}

bool hu_local_only_provider_endpoint_is_local(const char *provider_name, const char *base_url,
                                              int override) {
    if (override > 0)
        return true;
    if (override < 0)
        return false;
    if (name_in(provider_name, k_cloud_proxy_names,
                sizeof(k_cloud_proxy_names) / sizeof(k_cloud_proxy_names[0])))
        return false;
    if (base_url && base_url[0])
        return hu_provider_endpoint_is_local(base_url, strlen(base_url));
    return name_in(provider_name, k_in_process_names,
                   sizeof(k_in_process_names) / sizeof(k_in_process_names[0]));
}

/* ── Voice services (owner ruling 2026-10-02) ──────────────────────────── */

static bool path_has(const char *path, size_t len, const char *needle) {
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= len; i++)
        if (strncasecmp(path + i, needle, nl) == 0)
            return true;
    return false;
}

static bool path_ends(const char *path, size_t len, const char *suffix) {
    size_t sl = strlen(suffix);
    return len >= sl && strncasecmp(path + len - sl, suffix, sl) == 0;
}

bool hu_local_only_voice_service(const char *url, char *out, size_t cap) {
    if (!url || !out || cap < 8)
        return false;
    size_t ulen = strlen(url);
    const char *h = NULL;
    size_t hl = 0;
    if (!url_host(url, ulen, &h, &hl))
        return false;
    const char *path = h + hl;
    while (*path == ':' || (*path >= '0' && *path <= '9')) /* skip :port */
        path++;
    size_t plen = strcspn(path, "?#");
    while (plen > 0 && path[plen - 1] == '/')
        plen--;
    const char *kind = NULL;
    if (path_has(path, plen, "/tts/") || path_ends(path, plen, "/tts") ||
        path_ends(path, plen, "/audio/speech") || path_has(path, plen, "/text-to-speech"))
        kind = "tts";
    else if (path_has(path, plen, "/stt/") || path_ends(path, plen, "/stt") ||
             path_ends(path, plen, "/audio/transcriptions") ||
             path_ends(path, plen, "/audio/translations") || path_ends(path, plen, "/listen") ||
             path_ends(path, plen, "speech:recognize") ||
             path_ends(path, plen, "speech:longrunningrecognize"))
        kind = "stt";
    if (!kind)
        return false;
    /* Vendor: the registrable label (the one before the TLD). */
    size_t end = hl, dot_last = hl, dot_prev = (size_t)-1;
    for (size_t i = 0; i < end; i++) {
        if (h[i] == '.') {
            dot_prev = dot_last == hl ? (size_t)-1 : dot_last;
            dot_last = i;
        }
    }
    size_t vs = 0, ve = hl;
    if (dot_last != hl) {
        ve = dot_last;
        vs = dot_prev == (size_t)-1 ? 0 : dot_prev + 1;
    }
    char vendor[40];
    size_t vn = ve - vs < sizeof(vendor) - 1 ? ve - vs : sizeof(vendor) - 1;
    for (size_t i = 0; i < vn; i++) {
        char c = h[vs + i];
        vendor[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    vendor[vn] = '\0';
    int n = snprintf(out, cap, "%s:%s", kind, vendor);
    return n > 0 && (size_t)n < cap;
}

/* Reloadable policy: the voice allow-list and the provider endpoint
 * overrides. Published as an immutable snapshot through an atomic pointer,
 * so a reload never rewrites memory a reader is using. A replaced snapshot
 * is kept on a retired list (reachable, a few KB per reload) because readers
 * hold no reference count; hu_local_only_reset frees them (tests only, no
 * concurrent readers). Writers compare-and-swap, so two concurrent reloads
 * never lose an update. */
#define LO_LIST_MAX  16
#define LO_ITEM_SIZE 256

typedef struct lo_policy {
    size_t allow_n, vouched_n, vetoed_n;
    char allow[LO_LIST_MAX][LO_ITEM_SIZE];
    char vouched[LO_LIST_MAX][LO_ITEM_SIZE];
    char vetoed[LO_LIST_MAX][LO_ITEM_SIZE];
    struct lo_policy *retired_next;
} lo_policy_t;

static _Atomic(lo_policy_t *) g_policy = NULL;
static _Atomic(lo_policy_t *) g_retired = NULL;

static size_t copy_list(char dst[LO_LIST_MAX][LO_ITEM_SIZE], const char *const *items,
                        size_t count) {
    size_t n = 0;
    for (size_t i = 0; items && i < count && n < LO_LIST_MAX; i++) {
        if (!items[i] || !items[i][0] || strlen(items[i]) >= LO_ITEM_SIZE)
            continue;
        memcpy(dst[n], items[i], strlen(items[i]) + 1);
        n++;
    }
    return n;
}

/* Publish a copy of the current snapshot with one half replaced. */
static void policy_publish(bool set_allow, const char *const *allow, size_t allow_n,
                           const char *const *vouched, size_t vouched_n, const char *const *vetoed,
                           size_t vetoed_n) {
    lo_policy_t *next = (lo_policy_t *)calloc(1, sizeof(*next));
    if (!next)
        return; /* keep the previous policy */
    lo_policy_t *cur = atomic_load(&g_policy);
    for (;;) {
        if (cur)
            memcpy(next, cur, sizeof(*next));
        else
            memset(next, 0, sizeof(*next));
        next->retired_next = NULL;
        if (set_allow) {
            next->allow_n = copy_list(next->allow, allow, allow_n);
        } else {
            next->vouched_n = copy_list(next->vouched, vouched, vouched_n);
            next->vetoed_n = copy_list(next->vetoed, vetoed, vetoed_n);
        }
        if (atomic_compare_exchange_weak(&g_policy, &cur, next))
            break; /* cur reloaded on failure */
    }
    if (cur) {
        lo_policy_t *head = atomic_load(&g_retired);
        do {
            cur->retired_next = head;
        } while (!atomic_compare_exchange_weak(&g_retired, &head, cur));
    }
}

void hu_local_only_set_allow(const char *const *items, size_t count) {
    policy_publish(true, items, count, NULL, 0, NULL, 0);
}

void hu_local_only_set_endpoint_overrides(const char *const *vouched, size_t vouched_count,
                                          const char *const *vetoed, size_t vetoed_count) {
    policy_publish(false, NULL, 0, vouched, vouched_count, vetoed, vetoed_count);
}

bool hu_local_only_service_allowed(const char *service) {
    if (!service || !service[0])
        return false;
    const lo_policy_t *p = atomic_load(&g_policy);
    for (size_t i = 0; p && i < p->allow_n; i++)
        if (strcasecmp(p->allow[i], service) == 0)
            return true;
    return false;
}

static const char *after_scheme(const char *u) {
    const char *p = strstr(u, "://");
    return p ? p + 3 : u;
}

/* url starts with base at a path boundary (":4000" never covers ":40001").
 * The scheme is ignored, so an http base also covers its ws:// socket. */
static bool url_under_base(const char *url, const char *base) {
    url = after_scheme(url);
    base = after_scheme(base);
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/')
        bl--;
    if (bl == 0 || strncasecmp(url, base, bl) != 0)
        return false;
    char c = url[bl];
    return c == '\0' || c == '/' || c == '?' || c == '#';
}

bool hu_local_only_request_url_is_local(const char *url) {
    if (!url || !url[0])
        return false;
    const lo_policy_t *p = atomic_load(&g_policy);
    for (size_t i = 0; p && i < p->vetoed_n; i++)
        if (url_under_base(url, p->vetoed[i]))
            return false;
    for (size_t i = 0; p && i < p->vouched_n; i++)
        if (url_under_base(url, p->vouched[i]))
            return true;
    return hu_provider_endpoint_is_local(url, strlen(url));
}

/* ── Mode ──────────────────────────────────────────────────────────────── */

/* -2 = present but not a recognized value. */
static int env_parse_strict(const char *v) {
    if (!v || !v[0])
        return -1;
    if (!strcasecmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "false"))
        return (int)HU_GATE_OFF;
    if (!strcasecmp(v, "audit") || !strcasecmp(v, "shadow"))
        return (int)HU_GATE_SHADOW;
    if (!strcasecmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "live") ||
        !strcasecmp(v, "true") || !strcasecmp(v, "enforce"))
        return (int)HU_GATE_LIVE;
    return -2;
}

int hu_local_only_env_parse(const char *v) {
    int r = env_parse_strict(v);
    return r == -2 ? (int)HU_GATE_LIVE : r; /* unknown fails CLOSED */
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
    lo_policy_t *p = atomic_exchange(&g_policy, (lo_policy_t *)NULL);
    free(p);
    lo_policy_t *r = atomic_exchange(&g_retired, (lo_policy_t *)NULL);
    while (r) {
        lo_policy_t *nx = r->retired_next;
        free(r);
        r = nx;
    }
}

static void copy_ident(char *out, size_t cap, const char *s, size_t n);

hu_gate_mode_t hu_local_only_mode(void) {
    const char *v = getenv("HU_LOCAL_ONLY");
    int env = env_parse_strict(v);
    if (env == -2) {
        static atomic_bool warned = false;
        if (!atomic_exchange(&warned, true)) {
            char shown[32];
            copy_ident(shown, sizeof(shown), v, strlen(v));
            hu_log_warn("local_only", NULL,
                        "HU_LOCAL_ONLY=%s is not recognized (0|off|false, audit|shadow, "
                        "1|on|live|true|enforce) — failing closed: enforcing",
                        shown);
        }
        return HU_GATE_LIVE;
    }
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

const char *hu_local_only_current_caller(void) {
    return t_caller;
}

const char *hu_local_only_enter(const char *tag) {
    const char *prev = t_caller;
    if (!prev)
        t_caller = tag;
    return prev;
}

/* ── The checks ────────────────────────────────────────────────────────── */

/* Copy an identifier, keeping only [A-Za-z0-9._:-/]; never content. */
static void copy_ident(char *out, size_t cap, const char *s, size_t n) {
    size_t w = 0;
    for (size_t i = 0; s && i < n && w + 1 < cap; i++) {
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

/* The model id of a request: from ".../models/<m>:method" or the body's
 * leading "model" field. False when neither names one. */
static bool model_extract(const char *url, const char *body, size_t body_len, char *out,
                          size_t cap) {
    const char *m = strstr(url, "/models/");
    if (m) {
        m += 8;
        copy_ident(out, cap, m, strcspn(m, ":?#/"));
        return out[0] != '?';
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
                return out[0] != '?';
            }
        }
    }
    copy_ident(out, cap, "", 0);
    return false;
}

/* One WARN + counter per refused (or, in audit, would-be-refused) request.
 * Logs the host, an identifier (model or service) and the caller tag only. */
static hu_error_t lo_refuse(hu_gate_mode_t mode, const char *url, const char *what) {
    char host[96];
    const char *h = NULL;
    size_t hl = 0;
    if (url && url_host(url, strlen(url), &h, &hl))
        copy_ident(host, sizeof(host), h, hl);
    else
        copy_ident(host, sizeof(host), "", 0);
    char ident[64];
    copy_ident(ident, sizeof(ident), what, what ? strlen(what) : 0);
    const char *caller = t_caller ? t_caller : "unknown";
    if (mode == HU_GATE_SHADOW) {
        atomic_fetch_add(&g_audited, 1);
        hu_log_warn("local_only", NULL,
                    "[local_only audit] would refuse provider=%s model=%s caller=%s", host, ident,
                    caller);
        return HU_OK;
    }
    atomic_fetch_add(&g_refused, 1);
    hu_log_warn("local_only", NULL, "[local_only] refused provider=%s model=%s caller=%s", host,
                ident, caller);
    return HU_ERR_PERMISSION_DENIED;
}

hu_error_t hu_local_only_check_request(const char *url, const char *body, size_t body_len) {
    hu_gate_mode_t mode = hu_local_only_mode();
    if (mode == HU_GATE_OFF || !url)
        return HU_OK;
    bool model_req = hu_local_only_url_is_model_request(url);
    char svc[64];
    bool voice = !model_req && hu_local_only_voice_service(url, svc, sizeof(svc));
    if (!model_req && !voice)
        return HU_OK; /* feeds, channel APIs, OAuth */
    bool local = hu_local_only_request_url_is_local(url);
    if (voice)
        return (local || hu_local_only_service_allowed(svc)) ? HU_OK : lo_refuse(mode, url, svc);
    char model[64];
    bool have = model_extract(url, body, body_len, model, sizeof(model));
    if (local) /* a loopback gateway may still forward a cloud model */
        return (have && hu_local_only_model_name_is_cloud(model, strlen(model)))
                   ? lo_refuse(mode, url, model)
                   : HU_OK;
    return lo_refuse(mode, url, model);
}

hu_error_t hu_local_only_check_endpoint(const char *url) {
    hu_gate_mode_t mode = hu_local_only_mode();
    if (mode == HU_GATE_OFF)
        return HU_OK;
    if (!url || !url[0])
        return lo_refuse(mode, url, "endpoint");
    if (hu_local_only_request_url_is_local(url))
        return HU_OK;
    char svc[64];
    svc[0] = '\0';
    bool voice = hu_local_only_voice_service(url, svc, sizeof(svc));
    if (voice && hu_local_only_service_allowed(svc))
        return HU_OK;
    return lo_refuse(mode, url, voice ? svc : "endpoint");
}

hu_error_t hu_local_only_check_ws(const char *url) {
    return hu_local_only_check_endpoint(url);
}

hu_error_t hu_local_only_check_service(const char *service, const char *url) {
    hu_gate_mode_t mode = hu_local_only_mode();
    if (mode == HU_GATE_OFF)
        return HU_OK;
    if (hu_local_only_service_allowed(service))
        return HU_OK;
    return lo_refuse(mode, url, service);
}

uint64_t hu_local_only_refused_count(void) {
    return (uint64_t)atomic_load(&g_refused);
}

uint64_t hu_local_only_audit_count(void) {
    return (uint64_t)atomic_load(&g_audited);
}
