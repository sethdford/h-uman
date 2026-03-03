#include "seaclaw/subagent.h"
#include "seaclaw/agent.h"
#include "seaclaw/config.h"
#include "seaclaw/core/string.h"
#include "seaclaw/providers/factory.h"
#include "seaclaw/security.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
#include <pthread.h>
#endif

#define SC_MAX_TASKS              64
#define SC_DEFAULT_MAX_CONCURRENT 4

typedef struct sc_task_state {
    sc_task_status_t status;
    uint64_t task_id;
    char *label;
    char *task;
    char *result;
    char *error_msg;
    int64_t started_at;
    int64_t completed_at;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_t thread;
#endif
    bool thread_valid;
    volatile bool cancelled;
} sc_task_state_t;

struct sc_subagent_manager {
    sc_allocator_t *alloc;
    const sc_config_t *cfg;
    sc_task_state_t tasks[SC_MAX_TASKS];
    bool task_used[SC_MAX_TASKS];
    uint64_t next_id;
    uint32_t max_concurrent;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_t mutex;
#endif
};

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
typedef struct sc_thread_ctx {
    sc_subagent_manager_t *mgr;
    uint32_t slot;
} sc_thread_ctx_t;

static void *subagent_thread_fn(void *arg) {
    sc_thread_ctx_t *ctx = (sc_thread_ctx_t *)arg;
    sc_subagent_manager_t *mgr = ctx->mgr;
    uint32_t slot = ctx->slot;
    sc_allocator_t *alloc = mgr->alloc;
    sc_task_state_t *ts = &mgr->tasks[slot];
    char *result_str = NULL;

    if (ts->cancelled) {
        pthread_mutex_lock(&mgr->mutex);
        ts->status = SC_TASK_FAILED;
        ts->error_msg = sc_strndup(alloc, "cancelled", 9);
        ts->completed_at = (int64_t)time(NULL);
        ts->thread_valid = false;
        pthread_mutex_unlock(&mgr->mutex);
        alloc->free(alloc->ctx, ctx, sizeof(sc_thread_ctx_t));
        return NULL;
    }

#if defined(SC_GATEWAY_POSIX)
    if (mgr->cfg && mgr->cfg->default_provider && mgr->cfg->default_provider[0]) {
        const char *prov = mgr->cfg->default_provider;
        size_t prov_len = strlen(prov);
        const char *api_key = sc_config_default_provider_key(mgr->cfg);
        size_t api_key_len = api_key ? strlen(api_key) : 0;
        const char *base_url = sc_config_get_provider_base_url(mgr->cfg, prov);
        size_t base_url_len = base_url ? strlen(base_url) : 0;

        sc_provider_t provider = {0};
        if (sc_provider_create(alloc, prov, prov_len, api_key, api_key_len, base_url, base_url_len,
                               &provider) == SC_OK) {
            const char *model = mgr->cfg->default_model ? mgr->cfg->default_model : "";
            size_t model_len = strlen(model);
            double temp = mgr->cfg->temperature > 0.0 ? mgr->cfg->temperature : 0.7;
            const char *ws = mgr->cfg->workspace_dir ? mgr->cfg->workspace_dir : ".";
            size_t ws_len = strlen(ws);

            sc_agent_t agent = {0};
            if (sc_agent_from_config(&agent, alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                     model, model_len, prov, prov_len, temp, ws, ws_len, 25, 50,
                                     false, 2, NULL, 0) == SC_OK) {
                char *resp = NULL;
                size_t resp_len = 0;
                const char *sys = "You are a helpful assistant. Answer concisely.";
                sc_error_t err =
                    sc_agent_run_single(&agent, sys, strlen(sys), ts->task,
                                        ts->task ? strlen(ts->task) : 0, &resp, &resp_len);
                sc_agent_deinit(&agent);
                if (err == SC_OK && resp && resp_len > 0) {
                    result_str = sc_strndup(alloc, resp, resp_len);
                    alloc->free(alloc->ctx, resp, resp_len + 1);
                }
                if (ts->cancelled && !result_str) {
                    result_str = sc_strndup(alloc, "cancelled", 9);
                }
            }
            if (provider.vtable && provider.vtable->deinit)
                provider.vtable->deinit(provider.ctx, mgr->alloc);
        }
    }
#endif

    if (!result_str) {
        result_str = sc_strndup(alloc, ts->task, ts->task ? strlen(ts->task) : 0);
        if (!result_str)
            result_str = sc_strndup(alloc, "(no result)", 11);
    }

    pthread_mutex_lock(&mgr->mutex);
    ts->status = ts->cancelled ? SC_TASK_FAILED : SC_TASK_COMPLETED;
    ts->result = result_str;
    if (ts->cancelled && !ts->error_msg)
        ts->error_msg = sc_strndup(alloc, "cancelled", 9);
    ts->completed_at = (int64_t)time(NULL);
    ts->thread_valid = false;
    pthread_mutex_unlock(&mgr->mutex);

    alloc->free(alloc->ctx, ctx, sizeof(sc_thread_ctx_t));
    return NULL;
}
#endif

static uint32_t get_max_concurrent(const sc_config_t *cfg) {
    if (cfg && cfg->scheduler.max_concurrent > 0)
        return cfg->scheduler.max_concurrent;
    return SC_DEFAULT_MAX_CONCURRENT;
}

static size_t running_count_locked(sc_subagent_manager_t *mgr) {
    size_t count = 0;
    for (uint32_t i = 0; i < SC_MAX_TASKS; i++) {
        if (mgr->task_used[i] && mgr->tasks[i].status == SC_TASK_RUNNING)
            count++;
    }
    return count;
}

static sc_task_state_t *find_task_by_id(sc_subagent_manager_t *mgr, uint64_t task_id) {
    for (uint32_t i = 0; i < SC_MAX_TASKS; i++) {
        if (mgr->task_used[i] && mgr->tasks[i].task_id == task_id)
            return &mgr->tasks[i];
    }
    return NULL;
}

sc_subagent_manager_t *sc_subagent_create(sc_allocator_t *alloc, const sc_config_t *cfg) {
    if (!alloc)
        return NULL;

    sc_subagent_manager_t *mgr =
        (sc_subagent_manager_t *)alloc->alloc(alloc->ctx, sizeof(sc_subagent_manager_t));
    if (!mgr)
        return NULL;

    memset(mgr, 0, sizeof(*mgr));
    mgr->alloc = alloc;
    mgr->cfg = cfg;
    mgr->next_id = 1;
    mgr->max_concurrent = get_max_concurrent(cfg);

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    if (pthread_mutex_init(&mgr->mutex, NULL) != 0) {
        alloc->free(alloc->ctx, mgr, sizeof(sc_subagent_manager_t));
        return NULL;
    }
#endif

    return mgr;
}

void sc_subagent_destroy(sc_allocator_t *alloc, sc_subagent_manager_t *mgr) {
    if (!alloc || !mgr)
        return;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&mgr->mutex);
#endif

    for (uint32_t i = 0; i < SC_MAX_TASKS; i++) {
        if (!mgr->task_used[i])
            continue;

        sc_task_state_t *ts = &mgr->tasks[i];

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
        if (ts->thread_valid) {
            pthread_t th = ts->thread;
            ts->thread_valid = false;
            pthread_mutex_unlock(&mgr->mutex);
            pthread_join(th, NULL);
            pthread_mutex_lock(&mgr->mutex);
        }
#endif

        if (ts->label)
            alloc->free(alloc->ctx, ts->label, strlen(ts->label) + 1);
        if (ts->task)
            alloc->free(alloc->ctx, ts->task, strlen(ts->task) + 1);
        if (ts->result)
            alloc->free(alloc->ctx, ts->result, strlen(ts->result) + 1);
        if (ts->error_msg)
            alloc->free(alloc->ctx, ts->error_msg, strlen(ts->error_msg) + 1);

        mgr->task_used[i] = false;
    }

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&mgr->mutex);
    pthread_mutex_destroy(&mgr->mutex);
#endif

    alloc->free(alloc->ctx, mgr, sizeof(sc_subagent_manager_t));
}

sc_error_t sc_subagent_spawn(sc_subagent_manager_t *mgr, const char *task, size_t task_len,
                             const char *label, const char *channel, const char *chat_id,
                             uint64_t *out_task_id) {
    (void)channel;
    (void)chat_id;

    if (!mgr || !out_task_id)
        return SC_ERR_INVALID_ARGUMENT;

    sc_allocator_t *alloc = mgr->alloc;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&mgr->mutex);
#endif

    if (running_count_locked(mgr) >= mgr->max_concurrent) {
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
        pthread_mutex_unlock(&mgr->mutex);
#endif
        return SC_ERR_SUBAGENT_TOO_MANY;
    }

    uint32_t slot = SC_MAX_TASKS;
    for (uint32_t i = 0; i < SC_MAX_TASKS; i++) {
        if (!mgr->task_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot >= SC_MAX_TASKS) {
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
        pthread_mutex_unlock(&mgr->mutex);
#endif
        return SC_ERR_OUT_OF_MEMORY;
    }

    uint64_t task_id = mgr->next_id++;
    sc_task_state_t *ts = &mgr->tasks[slot];

    char *task_copy = sc_strndup(alloc, task, task_len);
    if (!task_copy) {
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
        pthread_mutex_unlock(&mgr->mutex);
#endif
        return SC_ERR_OUT_OF_MEMORY;
    }

    char *label_copy = label ? sc_strndup(alloc, label, strlen(label)) : NULL;
    /* label_copy can be NULL - we allow it */

    ts->task_id = task_id;
    ts->status = SC_TASK_RUNNING;
    ts->label = label_copy;
    ts->task = task_copy;
    ts->result = NULL;
    ts->error_msg = NULL;
    ts->started_at = (int64_t)time(NULL);
    ts->completed_at = 0;
    ts->thread_valid = false;
    ts->cancelled = false;
    mgr->task_used[slot] = true;

#if defined(SC_IS_TEST) && SC_IS_TEST == 1
    /* Test mode: synchronous, no thread. Set status = COMPLETED, result = "completed: <task>" */
    ts->result = sc_sprintf(alloc, "completed: %s", task_copy);
    if (!ts->result)
        ts->result = sc_strndup(alloc, "completed: (task)", 17);
    ts->status = SC_TASK_COMPLETED;
    ts->completed_at = (int64_t)time(NULL);
#else
    sc_thread_ctx_t *ctx = (sc_thread_ctx_t *)alloc->alloc(alloc->ctx, sizeof(sc_thread_ctx_t));
    if (!ctx) {
        if (ts->label)
            alloc->free(alloc->ctx, ts->label, strlen(ts->label) + 1);
        alloc->free(alloc->ctx, ts->task, strlen(ts->task) + 1);
        mgr->task_used[slot] = false;
        pthread_mutex_unlock(&mgr->mutex);
        return SC_ERR_OUT_OF_MEMORY;
    }
    ctx->mgr = mgr;
    ctx->slot = slot;

    if (pthread_create(&ts->thread, NULL, subagent_thread_fn, ctx) != 0) {
        alloc->free(alloc->ctx, ctx, sizeof(sc_thread_ctx_t));
        if (ts->label)
            alloc->free(alloc->ctx, ts->label, strlen(ts->label) + 1);
        alloc->free(alloc->ctx, ts->task, strlen(ts->task) + 1);
        mgr->task_used[slot] = false;
        pthread_mutex_unlock(&mgr->mutex);
        return SC_ERR_INTERNAL;
    }
    ts->thread_valid = true;
    pthread_mutex_unlock(&mgr->mutex);
#endif

    *out_task_id = task_id;
    return SC_OK;
}

sc_task_status_t sc_subagent_get_status(sc_subagent_manager_t *mgr, uint64_t task_id) {
    if (!mgr)
        return SC_TASK_FAILED;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&mgr->mutex);
#endif

    sc_task_state_t *ts = find_task_by_id(mgr, task_id);
    sc_task_status_t status = ts ? ts->status : SC_TASK_FAILED;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&mgr->mutex);
#endif

    return status;
}

const char *sc_subagent_get_result(sc_subagent_manager_t *mgr, uint64_t task_id) {
    if (!mgr)
        return NULL;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&mgr->mutex);
#endif

    sc_task_state_t *ts = find_task_by_id(mgr, task_id);
    const char *result = ts ? ts->result : NULL;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&mgr->mutex);
#endif

    return result;
}

size_t sc_subagent_running_count(sc_subagent_manager_t *mgr) {
    if (!mgr)
        return 0;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&mgr->mutex);
#endif

    size_t count = running_count_locked(mgr);

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&mgr->mutex);
#endif

    return count;
}

sc_error_t sc_subagent_cancel(sc_subagent_manager_t *mgr, uint64_t task_id) {
    if (!mgr)
        return SC_ERR_INVALID_ARGUMENT;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&mgr->mutex);
#endif

    sc_task_state_t *ts = find_task_by_id(mgr, task_id);
    if (!ts) {
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
        pthread_mutex_unlock(&mgr->mutex);
#endif
        return SC_ERR_NOT_FOUND;
    }
    ts->cancelled = true;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&mgr->mutex);
#endif
    return SC_OK;
}

sc_error_t sc_subagent_get_all(sc_subagent_manager_t *mgr, sc_allocator_t *alloc,
                               sc_subagent_task_info_t **out, size_t *out_count) {
    if (!mgr || !alloc || !out || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    size_t n = 0;
    for (uint32_t i = 0; i < SC_MAX_TASKS; i++) {
        if (mgr->task_used[i])
            n++;
    }
    if (n == 0)
        return SC_OK;

    sc_subagent_task_info_t *arr =
        (sc_subagent_task_info_t *)alloc->alloc(alloc->ctx, n * sizeof(sc_subagent_task_info_t));
    if (!arr)
        return SC_ERR_OUT_OF_MEMORY;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&mgr->mutex);
#endif

    size_t j = 0;
    for (uint32_t i = 0; i < SC_MAX_TASKS && j < n; i++) {
        if (!mgr->task_used[i])
            continue;
        sc_task_state_t *ts = &mgr->tasks[i];
        arr[j].task_id = ts->task_id;
        arr[j].status = ts->status;
        arr[j].label = ts->label;
        arr[j].task = ts->task;
        arr[j].started_at = ts->started_at;
        j++;
    }

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&mgr->mutex);
#endif

    *out = arr;
    *out_count = n;
    return SC_OK;
}
