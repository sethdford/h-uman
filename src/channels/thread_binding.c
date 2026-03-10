#include "human/channels/thread_binding.h"
#include "human/core/string.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
#include <pthread.h>
#endif

typedef struct hu_tb_entry {
    bool active;
    char *channel_name;
    char *thread_id;
    uint64_t agent_id;
    hu_thread_lifecycle_t lifecycle;
    int64_t created_at;
    int64_t last_active;
    uint32_t idle_timeout_secs;
} hu_tb_entry_t;

struct hu_thread_binding {
    hu_allocator_t *alloc;
    hu_tb_entry_t *entries;
    uint32_t max_bindings;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_t mu;
#endif
};

static hu_tb_entry_t *find_entry(hu_thread_binding_t *tb, const char *channel,
                                 const char *thread_id) {
    for (uint32_t i = 0; i < tb->max_bindings; i++) {
        hu_tb_entry_t *e = &tb->entries[i];
        if (e->active && e->lifecycle != HU_THREAD_CLOSED && e->channel_name && e->thread_id &&
            strcmp(e->channel_name, channel) == 0 && strcmp(e->thread_id, thread_id) == 0)
            return e;
    }
    return NULL;
}

static void entry_free(hu_allocator_t *a, hu_tb_entry_t *e) {
    if (e->channel_name) {
        a->free(a->ctx, e->channel_name, strlen(e->channel_name) + 1);
        e->channel_name = NULL;
    }
    if (e->thread_id) {
        a->free(a->ctx, e->thread_id, strlen(e->thread_id) + 1);
        e->thread_id = NULL;
    }
    e->active = false;
}

hu_thread_binding_t *hu_thread_binding_create(hu_allocator_t *alloc, uint32_t max_bindings) {
    if (!alloc || max_bindings == 0)
        return NULL;
    hu_thread_binding_t *tb = (hu_thread_binding_t *)alloc->alloc(alloc->ctx, sizeof(*tb));
    if (!tb)
        return NULL;
    memset(tb, 0, sizeof(*tb));
    tb->alloc = alloc;
    tb->max_bindings = max_bindings;
    tb->entries = (hu_tb_entry_t *)alloc->alloc(alloc->ctx, max_bindings * sizeof(hu_tb_entry_t));
    if (!tb->entries) {
        alloc->free(alloc->ctx, tb, sizeof(*tb));
        return NULL;
    }
    memset(tb->entries, 0, max_bindings * sizeof(hu_tb_entry_t));
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    if (pthread_mutex_init(&tb->mu, NULL) != 0) {
        alloc->free(alloc->ctx, tb->entries, max_bindings * sizeof(hu_tb_entry_t));
        alloc->free(alloc->ctx, tb, sizeof(*tb));
        return NULL;
    }
#endif
    return tb;
}

void hu_thread_binding_destroy(hu_thread_binding_t *tb) {
    if (!tb)
        return;
    for (uint32_t i = 0; i < tb->max_bindings; i++)
        if (tb->entries[i].active)
            entry_free(tb->alloc, &tb->entries[i]);
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_destroy(&tb->mu);
#endif
    tb->alloc->free(tb->alloc->ctx, tb->entries, tb->max_bindings * sizeof(hu_tb_entry_t));
    tb->alloc->free(tb->alloc->ctx, tb, sizeof(*tb));
}

hu_error_t hu_thread_binding_bind(hu_thread_binding_t *tb, const char *channel_name,
                                  const char *thread_id, uint64_t agent_id,
                                  uint32_t idle_timeout_secs) {
    if (!tb || !channel_name || !thread_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (find_entry(tb, channel_name, thread_id))
        return HU_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < tb->max_bindings; i++) {
        if (!tb->entries[i].active) {
            hu_tb_entry_t *e = &tb->entries[i];
            e->active = true;
            e->channel_name = hu_strndup(tb->alloc, channel_name, strlen(channel_name));
            e->thread_id = hu_strndup(tb->alloc, thread_id, strlen(thread_id));
            e->agent_id = agent_id;
            e->lifecycle = HU_THREAD_ACTIVE;
            e->created_at = (int64_t)time(NULL);
            e->last_active = e->created_at;
            e->idle_timeout_secs = idle_timeout_secs;
            return HU_OK;
        }
    }
    return HU_ERR_OUT_OF_MEMORY;
}

hu_error_t hu_thread_binding_unbind(hu_thread_binding_t *tb, const char *channel_name,
                                    const char *thread_id) {
    if (!tb || !channel_name || !thread_id)
        return HU_ERR_INVALID_ARGUMENT;
    hu_tb_entry_t *e = find_entry(tb, channel_name, thread_id);
    if (!e)
        return HU_ERR_NOT_FOUND;
    e->lifecycle = HU_THREAD_CLOSED;
    entry_free(tb->alloc, e);
    return HU_OK;
}

hu_error_t hu_thread_binding_lookup(hu_thread_binding_t *tb, const char *channel_name,
                                    const char *thread_id, uint64_t *out_agent_id) {
    if (!tb || !channel_name || !thread_id || !out_agent_id)
        return HU_ERR_INVALID_ARGUMENT;
    hu_tb_entry_t *e = find_entry(tb, channel_name, thread_id);
    if (!e)
        return HU_ERR_NOT_FOUND;
    *out_agent_id = e->agent_id;
    return HU_OK;
}

hu_error_t hu_thread_binding_touch(hu_thread_binding_t *tb, const char *channel_name,
                                   const char *thread_id) {
    if (!tb || !channel_name || !thread_id)
        return HU_ERR_INVALID_ARGUMENT;
    hu_tb_entry_t *e = find_entry(tb, channel_name, thread_id);
    if (!e)
        return HU_ERR_NOT_FOUND;
    e->last_active = (int64_t)time(NULL);
    return HU_OK;
}

size_t hu_thread_binding_expire_idle(hu_thread_binding_t *tb, int64_t now) {
    if (!tb)
        return 0;
    size_t expired = 0;
    for (uint32_t i = 0; i < tb->max_bindings; i++) {
        hu_tb_entry_t *e = &tb->entries[i];
        if (!e->active || e->lifecycle == HU_THREAD_CLOSED)
            continue;
        if (e->idle_timeout_secs > 0 && (now - e->last_active) >= (int64_t)e->idle_timeout_secs) {
            e->lifecycle = HU_THREAD_CLOSED;
            entry_free(tb->alloc, e);
            expired++;
        }
    }
    return expired;
}

hu_error_t hu_thread_binding_list(hu_thread_binding_t *tb, hu_allocator_t *alloc,
                                  hu_thread_binding_entry_t **out, size_t *out_count) {
    if (!tb || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    size_t n = 0;
    for (uint32_t i = 0; i < tb->max_bindings; i++)
        if (tb->entries[i].active && tb->entries[i].lifecycle != HU_THREAD_CLOSED)
            n++;
    if (n == 0)
        return HU_OK;
    hu_thread_binding_entry_t *arr =
        (hu_thread_binding_entry_t *)alloc->alloc(alloc->ctx, n * sizeof(*arr));
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;
    size_t j = 0;
    for (uint32_t i = 0; i < tb->max_bindings && j < n; i++) {
        hu_tb_entry_t *e = &tb->entries[i];
        if (!e->active || e->lifecycle == HU_THREAD_CLOSED)
            continue;
        arr[j].channel_name = e->channel_name;
        arr[j].thread_id = e->thread_id;
        arr[j].agent_id = e->agent_id;
        arr[j].lifecycle = e->lifecycle;
        arr[j].created_at = e->created_at;
        arr[j].last_active = e->last_active;
        arr[j].idle_timeout_secs = e->idle_timeout_secs;
        j++;
    }
    *out = arr;
    *out_count = n;
    return HU_OK;
}

size_t hu_thread_binding_count(hu_thread_binding_t *tb) {
    if (!tb)
        return 0;
    size_t n = 0;
    for (uint32_t i = 0; i < tb->max_bindings; i++)
        if (tb->entries[i].active && tb->entries[i].lifecycle != HU_THREAD_CLOSED)
            n++;
    return n;
}
