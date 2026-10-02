#include "human/providers/local_only.h"
#include <string.h>
#include <strings.h>

/* Private span kinds. One row per protected prompt section; see the header. */
static const hu_local_only_span_kind_t k_span_kinds[] = {
    {HU_LOCAL_ONLY_THREAD_BEGIN, HU_LOCAL_ONLY_THREAD_END},
};
#define SPAN_KIND_COUNT (sizeof(k_span_kinds) / sizeof(k_span_kinds[0]))

const hu_local_only_span_kind_t *hu_local_only_span_kinds(size_t *count) {
    if (count)
        *count = SPAN_KIND_COUNT;
    return k_span_kinds;
}

/* On-device backends only. HTTP names whose default URL is loopback
 * (mlx_local, ollama, llamacpp) count: pointing one at a remote host is an
 * owner's explicit choice of their own server. Anything else — including
 * "compatible", which is what an mlx_local instance reports from get_name —
 * is treated as cloud, so an unknown name strips (fails closed). */
static const char *const k_local_names[] = {
    "mlx_local", "mlx-local",          "mlx-http",         "mlx_http", "mlx",   "ollama",
    "llamacpp",  "llama.cpp",          "llama-cli",        "apple",    "apfel", "coreml",
    "embedded",  "apple-intelligence", "foundationmodels", "huml",
};

/* Model-name prefixes only a cloud API serves (case-insensitive). */
static const char *const k_cloud_model_prefixes[] = {
    "gemini", "gpt-", "chatgpt", "claude", "grok", "o1-", "o3-", "o4-",
};

bool hu_local_only_provider_name_is_local(const char *name) {
    if (!name || !name[0])
        return false;
    for (size_t i = 0; i < sizeof(k_local_names) / sizeof(k_local_names[0]); i++) {
        if (strcmp(name, k_local_names[i]) == 0)
            return true;
    }
    return false;
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
