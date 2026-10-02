#include "human/providers/local_only.h"
#include <string.h>

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

/* First BEGIN marker at or after `from` that starts a line. */
static bool find_begin(const char *s, size_t len, size_t from, size_t *at) {
    static const size_t blen = sizeof(HU_LOCAL_ONLY_BEGIN) - 1;
    for (size_t i = from; i + blen <= len; i++) {
        if ((i == 0 || s[i - 1] == '\n') && memcmp(s + i, HU_LOCAL_ONLY_BEGIN, blen) == 0) {
            *at = i;
            return true;
        }
    }
    return false;
}

/* End (exclusive) of the span that starts at `begin`: just past the END
 * line, or `len` when the END marker is missing (fail closed). */
static size_t span_end(const char *s, size_t len, size_t begin) {
    static const size_t elen = sizeof(HU_LOCAL_ONLY_END) - 1;
    for (size_t i = begin + 1; i + elen <= len; i++) {
        if (s[i - 1] == '\n' && memcmp(s + i, HU_LOCAL_ONLY_END, elen) == 0)
            return i + elen;
    }
    return len;
}

/* Walk the kept regions of `s`; copies them into `dst` when non-NULL.
 * Returns the kept byte count. */
static size_t copy_kept(const char *s, size_t len, size_t first, char *dst) {
    size_t w = 0;
    size_t cur = 0;
    size_t begin = first;
    for (;;) {
        if (dst)
            memcpy(dst + w, s + cur, begin - cur);
        w += begin - cur;
        cur = span_end(s, len, begin);
        if (cur >= len || !find_begin(s, len, cur, &begin))
            break;
    }
    if (cur < len) {
        if (dst)
            memcpy(dst + w, s + cur, len - cur);
        w += len - cur;
    }
    return w;
}

hu_error_t hu_local_only_strip(hu_allocator_t *alloc, const char *s, size_t len, char **out,
                               size_t *out_len) {
    if (!out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    size_t first = 0;
    if (!s || len == 0 || !find_begin(s, len, 0, &first))
        return HU_OK;
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    size_t w = copy_kept(s, len, first, NULL);
    char *buf = (char *)alloc->alloc(alloc->ctx, w + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    (void)copy_kept(s, len, first, buf);
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
    size_t at = 0;
    bool any = false;
    for (size_t i = 0; i < n && !any; i++) {
        const hu_chat_message_t *m = &in->messages[i];
        any = m->content && m->content_len > 0 && find_begin(m->content, m->content_len, 0, &at);
    }
    if (!any)
        return HU_OK;

    *use = NULL;
    scratch->msgs = (hu_chat_message_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_chat_message_t));
    scratch->owned = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
    scratch->owned_lens = (size_t *)alloc->alloc(alloc->ctx, n * sizeof(size_t));
    scratch->count = n;
    if (!scratch->msgs || !scratch->owned || !scratch->owned_lens) {
        hu_local_only_request_release(alloc, scratch);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(scratch->msgs, in->messages, n * sizeof(hu_chat_message_t));
    memset(scratch->owned, 0, n * sizeof(char *));
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
}

void hu_local_only_request_release(hu_allocator_t *alloc, hu_local_only_request_t *scratch) {
    if (!alloc || !scratch)
        return;
    if (scratch->owned) {
        for (size_t i = 0; i < scratch->count; i++) {
            if (scratch->owned[i])
                alloc->free(alloc->ctx, scratch->owned[i], scratch->owned_lens[i] + 1);
        }
        alloc->free(alloc->ctx, scratch->owned, scratch->count * sizeof(char *));
    }
    if (scratch->owned_lens)
        alloc->free(alloc->ctx, scratch->owned_lens, scratch->count * sizeof(size_t));
    if (scratch->msgs)
        alloc->free(alloc->ctx, scratch->msgs, scratch->count * sizeof(hu_chat_message_t));
    memset(scratch, 0, sizeof(*scratch));
}
