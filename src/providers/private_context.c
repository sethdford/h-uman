/* Private prompt blocks — contract: include/human/providers/private_context.h. */
#include "human/providers/private_context.h"
#include <string.h>

static const char *const k_private_headings[] = {
    HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT,
};
#define HU_PRIVATE_HEADING_COUNT (sizeof(k_private_headings) / sizeof(k_private_headings[0]))

/* Bounded search (no memmem in C11). */
static const char *find_bytes(const char *hay, size_t hay_len, const char *needle,
                              size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len)
        return NULL;
    for (size_t i = 0; i + needle_len <= hay_len; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0)
            return hay + i;
    return NULL;
}

/* First private block at or after `from`: [*start, *end). Only a heading at a
 * line start counts, so prose that merely quotes the heading is left alone. */
static bool next_block(const char *text, size_t len, size_t from, size_t *start, size_t *end) {
    size_t best = len;
    size_t best_hlen = 0;
    for (size_t h = 0; h < HU_PRIVATE_HEADING_COUNT; h++) {
        size_t hlen = strlen(k_private_headings[h]);
        size_t pos = from;
        while (pos < len) {
            const char *hit = find_bytes(text + pos, len - pos, k_private_headings[h], hlen);
            if (!hit)
                break;
            size_t at = (size_t)(hit - text);
            if (at == 0 || text[at - 1] == '\n') {
                if (at < best) {
                    best = at;
                    best_hlen = hlen;
                }
                break;
            }
            pos = at + 1;
        }
    }
    if (best == len)
        return false;
    size_t body = best + best_hlen;
    const char *blank = find_bytes(text + body, len - body, "\n\n", 2);
    *start = best;
    *end = blank ? (size_t)(blank - text) + 2 : len;
    return true;
}

bool hu_private_context_present(const char *text, size_t len) {
    size_t s = 0, e = 0;
    return text && next_block(text, len, 0, &s, &e);
}

size_t hu_private_context_strip(char *text, size_t len) {
    if (!text)
        return 0;
    size_t s = 0, e = 0;
    while (next_block(text, len, 0, &s, &e)) {
        memmove(text + s, text + e, len - e);
        len -= e - s;
    }
    text[len] = '\0';
    return len;
}

hu_error_t hu_private_context_strip_dup(hu_allocator_t *alloc, const char *text, size_t len,
                                        char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!hu_private_context_present(text, len))
        return HU_OK;
    char *copy = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!copy)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(copy, text, len);
    copy[len] = '\0';
    size_t n = hu_private_context_strip(copy, len);
    /* Shrinking keeps the allocation's recorded size honest for free(). */
    char *fit = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!fit) {
        alloc->free(alloc->ctx, copy, len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(fit, copy, n + 1);
    alloc->free(alloc->ctx, copy, len + 1);
    *out = fit;
    *out_len = n;
    return HU_OK;
}

void hu_private_context_release(hu_allocator_t *alloc, hu_private_request_t *scratch) {
    if (!alloc || !scratch)
        return;
    for (size_t i = 0; scratch->bodies && i < scratch->count; i++)
        if (scratch->bodies[i])
            alloc->free(alloc->ctx, scratch->bodies[i], scratch->body_lens[i] + 1);
    if (scratch->bodies)
        alloc->free(alloc->ctx, scratch->bodies, scratch->count * sizeof(char *));
    if (scratch->body_lens)
        alloc->free(alloc->ctx, scratch->body_lens, scratch->count * sizeof(size_t));
    if (scratch->msgs)
        alloc->free(alloc->ctx, scratch->msgs, scratch->count * sizeof(hu_chat_message_t));
    memset(scratch, 0, sizeof(*scratch));
}

const hu_chat_request_t *hu_private_context_redact_request(hu_allocator_t *alloc,
                                                           const hu_chat_request_t *req,
                                                           hu_private_request_t *scratch) {
    if (!alloc || !req || !scratch)
        return NULL;
    memset(scratch, 0, sizeof(*scratch));
    bool any = false;
    for (size_t i = 0; i < req->messages_count && !any; i++)
        any = req->messages[i].role == HU_ROLE_SYSTEM &&
              hu_private_context_present(req->messages[i].content, req->messages[i].content_len);
    if (!any)
        return req;
    size_t n = req->messages_count;
    scratch->count = n;
    scratch->msgs = (hu_chat_message_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_chat_message_t));
    scratch->bodies = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
    scratch->body_lens = (size_t *)alloc->alloc(alloc->ctx, n * sizeof(size_t));
    if (!scratch->msgs || !scratch->bodies || !scratch->body_lens) {
        hu_private_context_release(alloc, scratch);
        return NULL;
    }
    memset(scratch->bodies, 0, n * sizeof(char *));
    memset(scratch->body_lens, 0, n * sizeof(size_t));
    memcpy(scratch->msgs, req->messages, n * sizeof(hu_chat_message_t));
    for (size_t i = 0; i < n; i++) {
        if (scratch->msgs[i].role != HU_ROLE_SYSTEM)
            continue;
        if (hu_private_context_strip_dup(alloc, scratch->msgs[i].content,
                                         scratch->msgs[i].content_len, &scratch->bodies[i],
                                         &scratch->body_lens[i]) != HU_OK) {
            hu_private_context_release(alloc, scratch);
            return NULL;
        }
        if (scratch->bodies[i]) {
            scratch->msgs[i].content = scratch->bodies[i];
            scratch->msgs[i].content_len = scratch->body_lens[i];
        }
    }
    scratch->req = *req;
    scratch->req.messages = scratch->msgs;
    scratch->req.prompt_cache_id = NULL;
    scratch->req.prompt_cache_id_len = 0;
    return &scratch->req;
}
