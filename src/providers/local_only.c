#include "human/providers/local_only.h"
#include "human/core/log.h"
#include <string.h>
#include <strings.h>

/* Private span kinds. One row per protected prompt section; see the header. */
static const hu_local_only_span_kind_t k_span_kinds[] = {
    {HU_LOCAL_ONLY_THREAD_BEGIN, HU_LOCAL_ONLY_THREAD_END, "HU_THREAD_CONTEXT"},
};
#define SPAN_KIND_COUNT (sizeof(k_span_kinds) / sizeof(k_span_kinds[0]))

const hu_local_only_span_kind_t *hu_local_only_span_kinds(size_t *count) {
    if (count)
        *count = SPAN_KIND_COUNT;
    return k_span_kinds;
}

/* Model-name prefixes only a cloud API serves (case-insensitive). Bare
 * OpenAI o-series ("o1", "o3", "o4-mini") is matched separately. */
static const char *const k_cloud_model_prefixes[] = {
    "gemini", "gpt-", "chatgpt", "claude", "grok",
};

/* Backends whose base_url is a MODEL PATH (factory.c), never a host: always
 * local, decided before any URL parsing. */
static const char *const k_path_backends[] = {
    "coreml", "mlx", "embedded", "llama-cli", "llamacpp", "huml",
};

/* In-process backends that are local when no URL is configured (the Apple
 * family may point base_url at an apfel server, so a URL there decides). */
static const char *const k_in_process_names[] = {
    "apple", "apfel",    "apple-intelligence", "foundationmodels", "coreml",
    "mlx",   "embedded", "llama-cli",          "llamacpp",         "huml",
};

/* Gateways that forward to cloud APIs even when they listen on loopback. */
static const char *const k_cloud_proxy_names[] = {
    "openrouter", "litellm", "portkey", "helicone", "together", "groq", "fireworks", "requesty",
};

static bool name_in(const char *name, const char *const *list, size_t n) {
    if (!name || !name[0])
        return false;
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(name, list[i]) == 0)
            return true;
    }
    return false;
}
#define NAME_IN(name, list) name_in((name), (list), sizeof(list) / sizeof((list)[0]))

bool hu_local_only_provider_name_is_local(const char *name) {
    /* A name alone (no URL) is local only for an in-process backend. */
    return NAME_IN(name, k_in_process_names);
}

bool hu_local_only_provider_is_local(const hu_provider_t *prov) {
    if (!prov || !prov->vtable || !prov->vtable->get_name)
        return false;
    return hu_local_only_provider_name_is_local(prov->vtable->get_name(prov->ctx));
}

bool hu_local_only_model_is_cloud(const char *model, size_t model_len) {
    if (!model || model_len == 0)
        return false;
    /* "publishers/google/models/gemini-..." and "google/gemini-..." forms */
    for (size_t i = model_len; i > 0; i--) {
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

bool hu_local_only_attempt_is_local(bool provider_local, const char *model, size_t model_len) {
    return provider_local && !hu_local_only_model_is_cloud(model, model_len);
}

/* Earliest span start at or after `from` (a registered heading at a line
 * start); sets the kind. */
static bool find_begin(const char *s, size_t len, size_t from, size_t *at, size_t *kind) {
    for (size_t i = from; i < len; i++) {
        if (i != 0 && s[i - 1] != '\n')
            continue;
        for (size_t k = 0; k < SPAN_KIND_COUNT; k++) {
            size_t bl = strlen(k_span_kinds[k].begin);
            if (i + bl <= len && memcmp(s + i, k_span_kinds[k].begin, bl) == 0) {
                *at = i;
                *kind = k;
                return true;
            }
        }
    }
    return false;
}

/* End (exclusive) of the span of `kind` starting at `begin`: past its end
 * line, or past the first blank line for an end-less kind, or `len` when no
 * close exists (fail closed). */
static size_t span_end(const char *s, size_t len, size_t begin, size_t kind) {
    const char *end = k_span_kinds[kind].end;
    if (!end) {
        for (size_t i = begin + 1; i < len; i++) {
            if (s[i - 1] == '\n' && s[i] == '\n')
                return i + 1;
        }
        return len;
    }
    size_t el = strlen(end);
    for (size_t i = begin + 1; i + el <= len; i++) {
        if (s[i - 1] == '\n' && memcmp(s + i, end, el) == 0)
            return i + el;
    }
    return len;
}

/* Walk the kept regions of `s`; copies them into `dst` when non-NULL.
 * Returns the kept byte count. */
static size_t copy_kept(const char *s, size_t len, size_t first, size_t kind, char *dst) {
    size_t w = 0;
    size_t cur = 0;
    size_t begin = first;
    for (;;) {
        if (dst)
            memcpy(dst + w, s + cur, begin - cur);
        w += begin - cur;
        cur = span_end(s, len, begin, kind);
        if (cur >= len || !find_begin(s, len, cur, &begin, &kind))
            break;
    }
    if (cur < len) {
        if (dst)
            memcpy(dst + w, s + cur, len - cur);
        w += len - cur;
    }
    return w;
}

bool hu_local_only_has_span(const char *s, size_t len) {
    size_t at = 0, kind = 0;
    return s && len > 0 && find_begin(s, len, 0, &at, &kind);
}

hu_error_t hu_local_only_strip(hu_allocator_t *alloc, const char *s, size_t len, char **out,
                               size_t *out_len) {
    if (!out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    size_t first = 0, kind = 0;
    if (!s || len == 0 || !find_begin(s, len, 0, &first, &kind))
        return HU_OK;
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    size_t w = copy_kept(s, len, first, kind, NULL);
    char *buf = (char *)alloc->alloc(alloc->ctx, w + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    (void)copy_kept(s, len, first, kind, buf);
    buf[w] = '\0';
    *out = buf;
    *out_len = w;
    return HU_OK;
}

hu_error_t hu_local_only_request_prepare(hu_allocator_t *alloc, const hu_chat_request_t *in,
                                         hu_local_only_request_t *scratch,
                                         const hu_chat_request_t **use) {
    if (!scratch || !use)
        return HU_ERR_INVALID_ARGUMENT;
    memset(scratch, 0, sizeof(*scratch));
    *use = NULL;
    if (!in || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    *use = in;
    size_t n = in->messages_count;
    if (!in->messages || n == 0)
        return HU_OK;
    bool any = false;
    for (size_t i = 0; i < n && !any; i++)
        any = hu_local_only_has_span(in->messages[i].content, in->messages[i].content_len);
    if (!any)
        return HU_OK;

    /* Each array is zeroed the moment it exists, so a release after any
     * failed allocation below frees only what was actually allocated. */
    *use = NULL;
    scratch->count = n;
    scratch->msgs = (hu_chat_message_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_chat_message_t));
    if (!scratch->msgs)
        goto oom;
    memcpy(scratch->msgs, in->messages, n * sizeof(hu_chat_message_t));
    scratch->owned = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
    if (!scratch->owned)
        goto oom;
    memset(scratch->owned, 0, n * sizeof(char *));
    scratch->owned_lens = (size_t *)alloc->alloc(alloc->ctx, n * sizeof(size_t));
    if (!scratch->owned_lens)
        goto oom;
    memset(scratch->owned_lens, 0, n * sizeof(size_t));
    for (size_t i = 0; i < n; i++) {
        hu_chat_message_t *m = &scratch->msgs[i];
        char *stripped = NULL;
        size_t stripped_len = 0;
        hu_error_t err =
            hu_local_only_strip(alloc, m->content, m->content_len, &stripped, &stripped_len);
        if (err != HU_OK) {
            hu_local_only_request_release(alloc, scratch);
            return err;
        }
        if (stripped) {
            scratch->owned[i] = stripped;
            scratch->owned_lens[i] = stripped_len;
            m->content = stripped;
            m->content_len = stripped_len;
            scratch->stripped++;
        }
    }
    scratch->req = *in;
    scratch->req.messages = scratch->msgs;
    scratch->req.prompt_cache_id = NULL;
    scratch->req.prompt_cache_id_len = 0;
    *use = &scratch->req;
    return HU_OK;
oom:
    hu_local_only_request_release(alloc, scratch);
    return HU_ERR_OUT_OF_MEMORY;
}

void hu_local_only_request_release(hu_allocator_t *alloc, hu_local_only_request_t *scratch) {
    if (!alloc || !scratch)
        return;
    if (scratch->owned) {
        for (size_t i = 0; i < scratch->count; i++) {
            if (scratch->owned[i])
                alloc->free(alloc->ctx, scratch->owned[i],
                            (scratch->owned_lens ? scratch->owned_lens[i] : 0) + 1);
        }
        alloc->free(alloc->ctx, scratch->owned, scratch->count * sizeof(char *));
    }
    if (scratch->owned_lens)
        alloc->free(alloc->ctx, scratch->owned_lens, scratch->count * sizeof(size_t));
    if (scratch->msgs)
        alloc->free(alloc->ctx, scratch->msgs, scratch->count * sizeof(hu_chat_message_t));
    memset(scratch, 0, sizeof(*scratch));
}

/* Strict URL parse (aligned with #587): the scheme must open the string and
 * be http/https/ws/wss; nothing is searched for mid-string, so
 * "evil.com/?u=http://127.0.0.1" has no host at all. Returns the host of the
 * authority (after any "user@"; IPv6 inside brackets). */
static bool url_host(const char *url, const char **host, size_t *host_len) {
    static const char *const schemes[] = {"http://", "https://", "ws://", "wss://"};
    const char *p = NULL;
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        size_t sl = strlen(schemes[i]);
        if (strncasecmp(url, schemes[i], sl) == 0) {
            p = url + sl;
            break;
        }
    }
    if (!p)
        return false;
    const char *auth_end = p + strcspn(p, "/?#");
    for (const char *q = p; q < auth_end; q++) {
        if (*q == '@')
            p = q + 1;
    }
    if (*p == '[') {
        const char *close = memchr(p, ']', (size_t)(auth_end - p));
        if (!close)
            return false;
        *host = p + 1;
        *host_len = (size_t)(close - p - 1);
        return *host_len > 0;
    }
    size_t n = strcspn(p, ":/?#");
    *host = p;
    *host_len = n;
    return n > 0;
}

/* 127.a.b.c with four decimal octets, each 0-255. */
static bool host_is_ipv4_loopback(const char *h, size_t n) {
    if (n < 7 || memcmp(h, "127.", 4) != 0)
        return false;
    int octets = 0;
    size_t i = 0;
    while (i < n) {
        size_t start = i;
        unsigned v = 0;
        while (i < n && h[i] >= '0' && h[i] <= '9' && i - start < 3)
            v = v * 10 + (unsigned)(h[i++] - '0');
        if (i == start || v > 255)
            return false;
        octets++;
        if (i == n)
            break;
        if (h[i] != '.' || octets == 4)
            return false;
        i++;
        if (i == n)
            return false;
    }
    return octets == 4;
}

static bool host_is_loopback(const char *h, size_t n) {
    if (n == 9 && strncasecmp(h, "localhost", 9) == 0)
        return true;
    if (n > 10 && strncasecmp(h + n - 10, ".localhost", 10) == 0)
        return true;
    if ((n == 3 && memcmp(h, "::1", 3) == 0) || (n == 15 && memcmp(h, "0:0:0:0:0:0:0:1", 15) == 0))
        return true;
    return host_is_ipv4_loopback(h, n);
}

bool hu_local_only_url_is_local(const char *base_url) {
    if (!base_url || !base_url[0])
        return false;
    if (base_url[0] == '/' || strncasecmp(base_url, "unix:", 5) == 0 ||
        strncasecmp(base_url, "http+unix:", 10) == 0)
        return true;
    const char *h = NULL;
    size_t n = 0;
    return url_host(base_url, &h, &n) && host_is_loopback(h, n);
}

bool hu_local_only_endpoint_is_local(const char *provider_name, const char *base_url,
                                     int override) {
    if (override > 0)
        return true;
    if (override < 0)
        return false;
    if (NAME_IN(provider_name, k_cloud_proxy_names))
        return false;
    if (NAME_IN(provider_name, k_path_backends))
        return true;
    if (base_url && base_url[0])
        return hu_local_only_url_is_local(base_url);
    return NAME_IN(provider_name, k_in_process_names);
}

void hu_local_only_log_strip(const char *text, size_t len, bool endpoint_local, const char *model,
                             size_t model_len) {
    if (!text || len == 0)
        return;
    const char *reason = (endpoint_local && hu_local_only_model_is_cloud(model, model_len))
                             ? "stripped_cloud_model"
                             : "stripped_cloud_endpoint";
    for (size_t k = 0; k < SPAN_KIND_COUNT; k++) {
        const char *b = k_span_kinds[k].begin;
        size_t bl = strlen(b);
        for (size_t i = 0; i + bl <= len; i++) {
            if ((i == 0 || text[i - 1] == '\n') && memcmp(text + i, b, bl) == 0) {
                hu_log_info("local_only", NULL, "[%s] applied=0 reason=%s model=%.*s",
                            k_span_kinds[k].tag, reason, (int)(model ? model_len : 0),
                            model ? model : "");
                break;
            }
        }
    }
}

void hu_local_only_log_strip_request(const hu_chat_request_t *req, bool endpoint_local,
                                     const char *model, size_t model_len) {
    if (!req || !req->messages)
        return;
    for (size_t i = 0; i < req->messages_count; i++) {
        if (hu_local_only_has_span(req->messages[i].content, req->messages[i].content_len)) {
            hu_local_only_log_strip(req->messages[i].content, req->messages[i].content_len,
                                    endpoint_local, model, model_len);
            return;
        }
    }
}
