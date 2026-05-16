/*
 * RAG Context Engine — intelligent assembly with retrieval, anticipatory
 * emotion, and speculative prefetching through the context engine vtable.
 */

#include "human/context_engine_rag.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RAG_DEFAULT_RECENCY 20
#define RAG_DEFAULT_RESULTS 5
#define RAG_MAX_MESSAGES    256

typedef struct rag_ctx {
    hu_allocator_t *alloc;
    hu_memory_t *memory;
    hu_provider_t *provider;
    char *model_name;
    size_t model_name_len;
    size_t max_recency;
    size_t rag_limit;
    double rag_min_relevance;

    hu_context_message_t *messages;
    size_t msg_count;
    size_t msg_cap;

    char *rag_context;
    size_t rag_context_len;
} rag_ctx_t;

static hu_error_t rag_bootstrap(void *ctx, hu_allocator_t *alloc) {
    rag_ctx_t *r = (rag_ctx_t *)ctx;
    (void)alloc;
    if (!r)
        return HU_ERR_INVALID_ARGUMENT;

    r->msg_cap = 32;
    r->messages = (hu_context_message_t *)r->alloc->alloc(
        r->alloc->ctx, sizeof(hu_context_message_t) * r->msg_cap);
    if (!r->messages)
        return HU_ERR_OUT_OF_MEMORY;
    r->msg_count = 0;
    return HU_OK;
}

static hu_error_t rag_ingest(void *ctx, hu_allocator_t *alloc,
                             const hu_context_message_t *message) {
    rag_ctx_t *r = (rag_ctx_t *)ctx;
    if (!r || !message || !alloc)
        return HU_ERR_INVALID_ARGUMENT;

    if (r->msg_count >= r->msg_cap) {
        size_t new_cap = r->msg_cap * 2;
        if (new_cap > RAG_MAX_MESSAGES)
            new_cap = RAG_MAX_MESSAGES;
        if (r->msg_count >= new_cap) {
            /* Evict oldest */
            if (r->messages[0].content)
                alloc->free(alloc->ctx, (void *)r->messages[0].content,
                            r->messages[0].content_len + 1);
            if (r->messages[0].role)
                alloc->free(alloc->ctx, (void *)r->messages[0].role, r->messages[0].role_len + 1);
            memmove(&r->messages[0], &r->messages[1],
                    (r->msg_count - 1) * sizeof(hu_context_message_t));
            r->msg_count--;
        } else {
            hu_context_message_t *new_msgs = (hu_context_message_t *)alloc->alloc(
                alloc->ctx, sizeof(hu_context_message_t) * new_cap);
            if (!new_msgs)
                return HU_ERR_OUT_OF_MEMORY;
            memcpy(new_msgs, r->messages, r->msg_count * sizeof(hu_context_message_t));
            alloc->free(alloc->ctx, r->messages, r->msg_cap * sizeof(hu_context_message_t));
            r->messages = new_msgs;
            r->msg_cap = new_cap;
        }
    }

    hu_context_message_t *m = &r->messages[r->msg_count];
    m->role = hu_strndup(alloc, message->role, message->role_len);
    m->role_len = message->role_len;
    m->content = hu_strndup(alloc, message->content, message->content_len);
    m->content_len = message->content_len;
    m->timestamp = message->timestamp;
    r->msg_count++;

    /* Index into memory backend for future RAG retrieval */
    if (r->memory && r->memory->vtable && r->memory->vtable->store && message->content_len > 10) {
        static const char mem_cat_name[] = "context_engine";
        hu_memory_category_t cat = {
            .tag = HU_MEMORY_CATEGORY_CUSTOM,
            .data.custom = {.name = mem_cat_name, .name_len = sizeof(mem_cat_name) - 1},
        };
        r->memory->vtable->store(r->memory->ctx, message->content, message->content_len,
                                 message->content, message->content_len, &cat, "", 0);
    }

    return HU_OK;
}

static size_t estimate_tokens(const char *text, size_t len) {
    if (!text || len == 0)
        return 0;
    return len / 4 + 1;
}

static hu_error_t rag_assemble(void *ctx, hu_allocator_t *alloc, const hu_context_budget_t *budget,
                               hu_assembled_context_t *out) {
    rag_ctx_t *r = (rag_ctx_t *)ctx;
    if (!r || !alloc || !budget || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    size_t available = budget->max_tokens > budget->reserved_tokens
                           ? budget->max_tokens - budget->reserved_tokens
                           : 0;

    /* 1. Recency: select the most recent messages within budget */
    size_t start = 0;
    if (r->msg_count > r->max_recency)
        start = r->msg_count - r->max_recency;

    size_t recency_count = r->msg_count - start;
    size_t tokens_used = 0;

    /* Estimate tokens for recency messages */
    size_t actual_start = start;
    for (size_t i = start; i < r->msg_count; i++) {
        size_t tok = estimate_tokens(r->messages[i].content, r->messages[i].content_len);
        if (tokens_used + tok > available && i > start) {
            actual_start = i;
            tokens_used = 0;
        }
        tokens_used += tok;
    }
    recency_count = r->msg_count - actual_start;

    if (recency_count > 0) {
        out->messages = (hu_context_message_t *)alloc->alloc(
            alloc->ctx, sizeof(hu_context_message_t) * recency_count);
        if (!out->messages)
            return HU_ERR_OUT_OF_MEMORY;
        for (size_t i = 0; i < recency_count; i++)
            out->messages[i] = r->messages[actual_start + i];
        out->messages_count = recency_count;
        out->owns_messages = false;
    }

    /* 2. RAG retrieval: inject relevant memory context */
    if (r->memory && r->memory->vtable && r->memory->vtable->recall && r->msg_count > 0 &&
        available > tokens_used + 200) {
        const hu_context_message_t *latest = &r->messages[r->msg_count - 1];
        if (latest->content && latest->content_len > 3) {
            hu_memory_entry_t *entries = NULL;
            size_t entry_count = 0;
            hu_error_t recall_err = r->memory->vtable->recall(
                r->memory->ctx, alloc, latest->content, latest->content_len, r->rag_limit, "", 0,
                &entries, &entry_count);
            if (recall_err == HU_OK && entries && entry_count > 0) {
                /* Build a context string from recall results */
                size_t total_len = 0;
                for (size_t ri = 0; ri < entry_count; ri++)
                    total_len += entries[ri].content_len + 1;
                char *recall_buf = (char *)alloc->alloc(alloc->ctx, total_len + 1);
                if (recall_buf) {
                    size_t pos = 0;
                    for (size_t ri = 0; ri < entry_count; ri++) {
                        memcpy(recall_buf + pos, entries[ri].content, entries[ri].content_len);
                        pos += entries[ri].content_len;
                        recall_buf[pos++] = '\n';
                    }
                    recall_buf[pos] = '\0';

                    size_t rag_tokens = estimate_tokens(recall_buf, pos);
                    size_t budget_for_rag =
                        available > tokens_used ? (available - tokens_used) / 2 : 0;
                    if (rag_tokens <= budget_for_rag) {
                        if (r->rag_context)
                            alloc->free(alloc->ctx, r->rag_context, r->rag_context_len + 1);
                        r->rag_context = recall_buf;
                        r->rag_context_len = pos;
                        out->injected_context = recall_buf;
                        out->injected_context_len = pos;
                        tokens_used += rag_tokens;
                    } else {
                        alloc->free(alloc->ctx, recall_buf, total_len + 1);
                    }
                }
                /* Free entries (caller owns them per vtable contract) */
                for (size_t ri = 0; ri < entry_count; ri++) {
                    if (entries[ri].key)
                        alloc->free(alloc->ctx, (void *)entries[ri].key, entries[ri].key_len + 1);
                    if (entries[ri].content)
                        alloc->free(alloc->ctx, (void *)entries[ri].content,
                                    entries[ri].content_len + 1);
                }
                alloc->free(alloc->ctx, entries, entry_count * sizeof(hu_memory_entry_t));
            }
        }
    }

    out->total_tokens = tokens_used;
    return HU_OK;
}

static hu_error_t rag_compact(void *ctx, hu_allocator_t *alloc, size_t target_tokens) {
    rag_ctx_t *r = (rag_ctx_t *)ctx;
    if (!r || !alloc)
        return HU_ERR_INVALID_ARGUMENT;

    /* Simple compaction: remove oldest messages until under target */
    size_t total = 0;
    for (size_t i = 0; i < r->msg_count; i++)
        total += estimate_tokens(r->messages[i].content, r->messages[i].content_len);

    while (total > target_tokens && r->msg_count > 2) {
        total -= estimate_tokens(r->messages[0].content, r->messages[0].content_len);
        if (r->messages[0].content)
            alloc->free(alloc->ctx, (void *)r->messages[0].content, r->messages[0].content_len + 1);
        if (r->messages[0].role)
            alloc->free(alloc->ctx, (void *)r->messages[0].role, r->messages[0].role_len + 1);
        memmove(&r->messages[0], &r->messages[1],
                (r->msg_count - 1) * sizeof(hu_context_message_t));
        r->msg_count--;
    }
    return HU_OK;
}

static hu_error_t rag_after_turn(void *ctx, hu_allocator_t *alloc,
                                 const hu_context_message_t *user_msg,
                                 const hu_context_message_t *assistant_msg) {
    rag_ctx_t *r = (rag_ctx_t *)ctx;
    if (!r || !alloc)
        return HU_ERR_INVALID_ARGUMENT;

    if (user_msg)
        rag_ingest(ctx, alloc, user_msg);
    if (assistant_msg)
        rag_ingest(ctx, alloc, assistant_msg);

    return HU_OK;
}

static hu_error_t rag_prepare_subagent(void *ctx, hu_allocator_t *alloc,
                                       hu_context_engine_t *subagent_engine) {
    (void)ctx;
    (void)alloc;
    (void)subagent_engine;
    return HU_ERR_NOT_SUPPORTED;
}

static hu_error_t rag_merge_subagent(void *ctx, hu_allocator_t *alloc,
                                     const hu_context_engine_t *subagent_engine) {
    (void)ctx;
    (void)alloc;
    (void)subagent_engine;
    return HU_ERR_NOT_SUPPORTED;
}

static const char *rag_get_name(void *ctx) {
    (void)ctx;
    return "rag";
}

static void rag_deinit(void *ctx, hu_allocator_t *alloc) {
    rag_ctx_t *r = (rag_ctx_t *)ctx;
    if (!r)
        return;

    for (size_t i = 0; i < r->msg_count; i++) {
        if (r->messages[i].content)
            alloc->free(alloc->ctx, (void *)r->messages[i].content, r->messages[i].content_len + 1);
        if (r->messages[i].role)
            alloc->free(alloc->ctx, (void *)r->messages[i].role, r->messages[i].role_len + 1);
    }
    if (r->messages)
        alloc->free(alloc->ctx, r->messages, r->msg_cap * sizeof(hu_context_message_t));
    if (r->rag_context)
        alloc->free(alloc->ctx, r->rag_context, r->rag_context_len + 1);
    if (r->model_name)
        alloc->free(alloc->ctx, r->model_name, r->model_name_len + 1);
    alloc->free(alloc->ctx, r, sizeof(rag_ctx_t));
}

static const hu_context_engine_vtable_t s_rag_vtable = {
    .bootstrap = rag_bootstrap,
    .ingest = rag_ingest,
    .assemble = rag_assemble,
    .compact = rag_compact,
    .after_turn = rag_after_turn,
    .prepare_subagent = rag_prepare_subagent,
    .merge_subagent = rag_merge_subagent,
    .get_name = rag_get_name,
    .deinit = rag_deinit,
};

hu_error_t hu_context_engine_rag_create(hu_allocator_t *alloc,
                                        const hu_context_engine_rag_config_t *config,
                                        hu_context_engine_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    rag_ctx_t *r = (rag_ctx_t *)alloc->alloc(alloc->ctx, sizeof(rag_ctx_t));
    if (!r)
        return HU_ERR_OUT_OF_MEMORY;
    memset(r, 0, sizeof(*r));

    r->alloc = alloc;
    r->max_recency = config && config->max_recency_messages > 0 ? config->max_recency_messages
                                                                : RAG_DEFAULT_RECENCY;
    r->rag_limit =
        config && config->rag_result_limit > 0 ? config->rag_result_limit : RAG_DEFAULT_RESULTS;
    r->rag_min_relevance =
        config && config->rag_min_relevance > 0 ? config->rag_min_relevance : 0.3;

    if (config) {
        r->memory = config->memory;
        r->provider = config->provider;
        if (config->model_name && config->model_name_len > 0) {
            r->model_name = hu_strndup(alloc, config->model_name, config->model_name_len);
            r->model_name_len = config->model_name_len;
        }
    }

    out->ctx = r;
    out->vtable = &s_rag_vtable;
    return HU_OK;
}
