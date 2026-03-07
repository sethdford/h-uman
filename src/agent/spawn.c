#include "seaclaw/agent/spawn.h"
#include "seaclaw/agent.h"
#include "seaclaw/core/error.h"
#include "seaclaw/agent/mailbox.h"
#include "seaclaw/agent/team.h"
#include "seaclaw/agent/worktree.h"
#include "seaclaw/core/string.h"
#include "seaclaw/providers/factory.h"
#include "seaclaw/security.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
#include <pthread.h>
#endif

#define SC_POOL_MAX_SLOTS    64
#define SC_POOL_DEFAULT_ITER 25

typedef struct sc_pool_slot {
    uint64_t agent_id;
    sc_agent_status_t status;
    sc_spawn_mode_t mode;
    char *label;
    char *model;
    char *task;
    char *result;
    char *provider_name;
    char *api_key;
    char *base_url;
    char *workspace_dir;
    char *system_prompt;
    char *persona_name;
    double temperature;
    uint32_t max_iterations;
    sc_security_policy_t *policy;
    sc_mailbox_t *mailbox;
    int64_t started_at;
    double cost_usd;
    volatile bool cancelled;
    void *persistent_agent;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_t thread;
#endif
    bool thread_valid;
} sc_pool_slot_t;

struct sc_agent_pool {
    sc_allocator_t *alloc;
    sc_pool_slot_t slots[SC_POOL_MAX_SLOTS];
    bool used[SC_POOL_MAX_SLOTS];
    uint64_t next_id;
    uint32_t max_concurrent;
    sc_worktree_manager_t *worktree_mgr;
    sc_team_config_t *team_config;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_t mu;
#endif
};

static size_t running_locked(sc_agent_pool_t *p) {
    size_t n = 0;
    for (uint32_t i = 0; i < SC_POOL_MAX_SLOTS; i++)
        if (p->used[i] && p->slots[i].status == SC_AGENT_RUNNING)
            n++;
    return n;
}

static sc_pool_slot_t *find_slot(sc_agent_pool_t *p, uint64_t id) {
    for (uint32_t i = 0; i < SC_POOL_MAX_SLOTS; i++)
        if (p->used[i] && p->slots[i].agent_id == id)
            return &p->slots[i];
    return NULL;
}

static void free_str(sc_allocator_t *a, char **s) {
    if (*s) {
        a->free(a->ctx, *s, strlen(*s) + 1);
        *s = NULL;
    }
}

static void slot_free(sc_allocator_t *a, sc_pool_slot_t *s) {
    free_str(a, &s->label);
    free_str(a, &s->model);
    free_str(a, &s->task);
    free_str(a, &s->result);
    free_str(a, &s->provider_name);
    free_str(a, &s->api_key);
    free_str(a, &s->base_url);
    free_str(a, &s->workspace_dir);
    free_str(a, &s->system_prompt);
    free_str(a, &s->persona_name);
}

static void slot_deinit_agent(sc_allocator_t *a, sc_pool_slot_t *s) {
    if (s->persistent_agent) {
        sc_agent_t *ag = (sc_agent_t *)s->persistent_agent;
        sc_agent_deinit(ag);
        a->free(a->ctx, ag, sizeof(sc_agent_t));
        s->persistent_agent = NULL;
    }
}

static char *dup_opt(sc_allocator_t *a, const char *s, size_t len) {
    if (!s || len == 0)
        return NULL;
    return sc_strndup(a, s, len);
}

#if defined(SC_GATEWAY_POSIX) && (!defined(SC_IS_TEST) || SC_IS_TEST == 0)
typedef struct {
    sc_agent_pool_t *pool;
    uint32_t slot;
} sc_spawn_tctx_t;

static void *spawn_thread(void *arg) {
    sc_spawn_tctx_t *tc = (sc_spawn_tctx_t *)arg;
    sc_agent_pool_t *pool = tc->pool;
    sc_pool_slot_t *s = &pool->slots[tc->slot];
    sc_allocator_t *a = pool->alloc;
    char *result = NULL;
    sc_provider_t prov = {0};
    bool created_prov = false;

    if (s->cancelled)
        goto done;

    const char *pn = s->provider_name ? s->provider_name : "openai";
    const char *ak = s->api_key ? s->api_key : "";
    const char *bu = s->base_url ? s->base_url : "";
    if (sc_provider_create(a, pn, strlen(pn), ak, strlen(ak), bu, strlen(bu), &prov) != SC_OK) {
        result = sc_strndup(a, "(provider create failed)", 24);
        goto done;
    }
    created_prov = true;

    const char *mdl = (s->model && s->model[0]) ? s->model : "gpt-4o-mini";
    double temp = s->temperature > 0.0 ? s->temperature : 0.7;
    const char *ws = (s->workspace_dir && s->workspace_dir[0]) ? s->workspace_dir : ".";
    uint32_t mi = s->max_iterations > 0 ? s->max_iterations : SC_POOL_DEFAULT_ITER;
    const char *sys = (s->system_prompt && s->system_prompt[0]) ? s->system_prompt
                                                                : "You are a helpful assistant.";
    const char *task = s->task ? s->task : "";
    size_t task_len = s->task ? strlen(s->task) : 0;

    if (s->mode == SC_SPAWN_PERSISTENT) {
        sc_agent_t *ag = (sc_agent_t *)a->alloc(a->ctx, sizeof(sc_agent_t));
        if (!ag) {
            result = sc_strndup(a, "(oom)", 5);
            goto done;
        }
        memset(ag, 0, sizeof(*ag));
        if (sc_agent_from_config(ag, a, prov, NULL, 0, NULL, NULL, NULL, s->policy, mdl,
                                 strlen(mdl), pn, strlen(pn), temp, ws, strlen(ws), mi, 50, false,
                                 2, sys, strlen(sys), s->persona_name,
                                 s->persona_name ? strlen(s->persona_name) : 0, NULL) != SC_OK) {
            a->free(a->ctx, ag, sizeof(*ag));
            result = sc_strndup(a, "(agent create failed)", 21);
            goto done;
        }
        ag->agent_id = s->agent_id;
        ag->worktree_mgr = pool->worktree_mgr;
        if (s->mailbox)
            sc_agent_set_mailbox(ag, s->mailbox);
        char *resp = NULL;
        size_t rlen = 0;
        sc_error_t e = sc_agent_turn(ag, task, task_len, &resp, &rlen);
        if (e == SC_OK && resp && rlen > 0) {
            result = sc_strndup(a, resp, rlen);
            a->free(a->ctx, resp, rlen + 1);
        } else if (resp) {
            a->free(a->ctx, resp, rlen + 1);
        }
        if (!result)
            result = sc_strndup(a, "(no response)", 13);

        pthread_mutex_lock(&pool->mu);
        if (s->cancelled) {
            sc_agent_deinit(ag);
            a->free(a->ctx, ag, sizeof(*ag));
            s->status = SC_AGENT_CANCELLED;
        } else {
            s->persistent_agent = ag;
            s->result = result;
            result = NULL;
            s->status = SC_AGENT_IDLE;
        }
        pthread_mutex_unlock(&pool->mu);
    } else {
        sc_agent_t ag = {0};
        if (sc_agent_from_config(&ag, a, prov, NULL, 0, NULL, NULL, NULL, s->policy, mdl,
                                 strlen(mdl), pn, strlen(pn), temp, ws, strlen(ws), mi, 50, false,
                                 2, sys, strlen(sys), s->persona_name,
                                 s->persona_name ? strlen(s->persona_name) : 0, NULL) != SC_OK) {
            result = sc_strndup(a, "(agent create failed)", 21);
            goto done;
        }
        ag.agent_id = s->agent_id;
        ag.worktree_mgr = pool->worktree_mgr;
        if (s->mailbox)
            sc_agent_set_mailbox(&ag, s->mailbox);
        char *resp = NULL;
        size_t rlen = 0;
        sc_error_t turn_err = sc_agent_turn(&ag, task, task_len, &resp, &rlen);
        if (resp && rlen > 0) {
            result = sc_strndup(a, resp, rlen);
            a->free(a->ctx, resp, rlen + 1);
        } else if (resp) {
            a->free(a->ctx, resp, rlen + 1);
        }
        sc_agent_deinit(&ag);
        if (!result) {
            const char *err_str = sc_error_string(turn_err);
            result = sc_strndup(a, err_str, strlen(err_str));
        }
        if (pool->worktree_mgr) {
            sc_error_t rm_err = sc_worktree_remove(pool->worktree_mgr, s->agent_id);
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
            if (rm_err != SC_OK)
                fprintf(stderr, "[spawn] worktree cleanup failed: %s\n", sc_error_string(rm_err));
#else
            (void)rm_err;
#endif
        }
    }

done:
    if (created_prov && prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, a);

    pthread_mutex_lock(&pool->mu);
    if (s->status != SC_AGENT_IDLE && s->status != SC_AGENT_CANCELLED) {
        s->status = s->cancelled ? SC_AGENT_CANCELLED : SC_AGENT_COMPLETED;
        if (result) {
            s->result = result;
            result = NULL;
        }
    }
    if (result)
        a->free(a->ctx, result, strlen(result) + 1);
    s->thread_valid = false;
    pthread_mutex_unlock(&pool->mu);
    a->free(a->ctx, tc, sizeof(*tc));
    return NULL;
}
#endif

sc_agent_pool_t *sc_agent_pool_create(sc_allocator_t *alloc, uint32_t max_concurrent) {
    if (!alloc)
        return NULL;
    sc_agent_pool_t *p = (sc_agent_pool_t *)alloc->alloc(alloc->ctx, sizeof(*p));
    if (!p)
        return NULL;
    memset(p, 0, sizeof(*p));
    p->alloc = alloc;
    p->next_id = 1;
    p->max_concurrent = max_concurrent > 0 ? max_concurrent : 4;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    if (pthread_mutex_init(&p->mu, NULL) != 0) {
        alloc->free(alloc->ctx, p, sizeof(*p));
        return NULL;
    }
#endif
    return p;
}

void sc_agent_pool_set_worktree_manager(sc_agent_pool_t *pool,
                                        sc_worktree_manager_t *worktree_mgr) {
    if (!pool)
        return;
    pool->worktree_mgr = worktree_mgr;
}

void sc_agent_pool_set_team_config(sc_agent_pool_t *pool, sc_team_config_t *team_config) {
    if (!pool)
        return;
    pool->team_config = team_config;
}

void sc_agent_pool_destroy(sc_agent_pool_t *pool) {
    if (!pool)
        return;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    for (uint32_t i = 0; i < SC_POOL_MAX_SLOTS; i++) {
        if (!pool->used[i])
            continue;
        sc_pool_slot_t *s = &pool->slots[i];
        if (pool->worktree_mgr && s->agent_id) {
            sc_error_t rm_err = sc_worktree_remove(pool->worktree_mgr, s->agent_id);
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
            if (rm_err != SC_OK)
                fprintf(stderr, "[spawn] worktree cleanup failed: %s\n", sc_error_string(rm_err));
#else
            (void)rm_err;
#endif
        }
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
        if (s->thread_valid) {
            pthread_t th = s->thread;
            s->thread_valid = false;
            pthread_mutex_unlock(&pool->mu);
            pthread_join(th, NULL);
            pthread_mutex_lock(&pool->mu);
        }
#endif
        slot_deinit_agent(pool->alloc, s);
        slot_free(pool->alloc, s);
        pool->used[i] = false;
    }
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
    pthread_mutex_destroy(&pool->mu);
#endif
    pool->alloc->free(pool->alloc->ctx, pool, sizeof(*pool));
}

sc_error_t sc_agent_pool_spawn(sc_agent_pool_t *pool, const sc_spawn_config_t *cfg,
                               const char *task, size_t task_len, const char *label,
                               uint64_t *out_id) {
    if (!pool || !cfg || !out_id)
        return SC_ERR_INVALID_ARGUMENT;
    sc_allocator_t *a = pool->alloc;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    if (running_locked(pool) >= pool->max_concurrent) {
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
        pthread_mutex_unlock(&pool->mu);
#endif
        return SC_ERR_SUBAGENT_TOO_MANY;
    }
    uint32_t si = SC_POOL_MAX_SLOTS;
    for (uint32_t i = 0; i < SC_POOL_MAX_SLOTS; i++)
        if (!pool->used[i]) {
            si = i;
            break;
        }
    if (si >= SC_POOL_MAX_SLOTS) {
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
        pthread_mutex_unlock(&pool->mu);
#endif
        return SC_ERR_OUT_OF_MEMORY;
    }

    sc_pool_slot_t *s = &pool->slots[si];
    memset(s, 0, sizeof(*s));
    s->agent_id = pool->next_id++;
    s->status = SC_AGENT_RUNNING;
    s->mode = cfg->mode;
    s->started_at = (int64_t)time(NULL);
    s->temperature = cfg->temperature;
    s->max_iterations = cfg->max_iterations;
    s->policy = cfg->policy;
    s->provider_name = dup_opt(a, cfg->provider, cfg->provider_len);
    s->api_key = dup_opt(a, cfg->api_key, cfg->api_key_len);
    s->base_url = dup_opt(a, cfg->base_url, cfg->base_url_len);
    s->model = dup_opt(a, cfg->model, cfg->model_len);
    if (pool->worktree_mgr) {
        const char *wt_path = NULL;
        const char *lbl = (label && label[0]) ? label : "agent";
        if (sc_worktree_create(pool->worktree_mgr, s->agent_id, lbl, &wt_path) == SC_OK &&
            wt_path) {
            s->workspace_dir = sc_strdup(a, wt_path);
        } else {
            s->workspace_dir = dup_opt(a, cfg->workspace_dir, cfg->workspace_dir_len);
        }
    } else {
        s->workspace_dir = dup_opt(a, cfg->workspace_dir, cfg->workspace_dir_len);
    }
    s->system_prompt = dup_opt(a, cfg->system_prompt, cfg->system_prompt_len);
    s->persona_name = dup_opt(a, cfg->persona_name, cfg->persona_name_len);
    s->task = task ? sc_strndup(a, task, task_len) : NULL;
    s->label = label ? sc_strndup(a, label, strlen(label)) : NULL;
    s->mailbox = cfg->mailbox;
    pool->used[si] = true;

#if defined(SC_IS_TEST) && SC_IS_TEST == 1
    s->result = sc_sprintf(a, "(spawned: %s)", s->label ? s->label : "agent");
    if (!s->result)
        s->result = sc_strndup(a, "(spawned)", 9);
    s->status = (cfg->mode == SC_SPAWN_PERSISTENT) ? SC_AGENT_IDLE : SC_AGENT_COMPLETED;
    if (pool->worktree_mgr && s->status == SC_AGENT_COMPLETED) {
        sc_error_t rm_err = sc_worktree_remove(pool->worktree_mgr, s->agent_id);
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
        if (rm_err != SC_OK)
            fprintf(stderr, "[spawn] worktree cleanup failed: %s\n", sc_error_string(rm_err));
#else
        (void)rm_err;
#endif
    }
#elif defined(SC_GATEWAY_POSIX)
    {
        sc_spawn_tctx_t *tc = (sc_spawn_tctx_t *)a->alloc(a->ctx, sizeof(*tc));
        if (!tc) {
            slot_free(a, s);
            pool->used[si] = false;
            pthread_mutex_unlock(&pool->mu);
            return SC_ERR_OUT_OF_MEMORY;
        }
        tc->pool = pool;
        tc->slot = si;
        if (pthread_create(&s->thread, NULL, spawn_thread, tc) != 0) {
            a->free(a->ctx, tc, sizeof(*tc));
            slot_free(a, s);
            pool->used[si] = false;
            pthread_mutex_unlock(&pool->mu);
            return SC_ERR_INTERNAL;
        }
        s->thread_valid = true;
    }
    pthread_mutex_unlock(&pool->mu);
#else
    s->result = sc_sprintf(a, "(spawned: %s)", s->label ? s->label : "agent");
    if (!s->result)
        s->result = sc_strndup(a, "(spawned)", 9);
    s->status = (cfg->mode == SC_SPAWN_PERSISTENT) ? SC_AGENT_IDLE : SC_AGENT_COMPLETED;
#endif

    *out_id = s->agent_id;
    return SC_OK;
}

sc_error_t sc_agent_pool_query(sc_agent_pool_t *pool, uint64_t agent_id, const char *message,
                               size_t message_len, char **out_response, size_t *out_response_len) {
    if (!pool || !out_response || !out_response_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out_response = NULL;
    *out_response_len = 0;

#if defined(SC_IS_TEST) && SC_IS_TEST == 1
    (void)agent_id;
    (void)message;
    (void)message_len;
    *out_response = sc_strndup(pool->alloc, "(query response)", 16);
    *out_response_len = 16;
    return SC_OK;
#else
#if !defined(SC_GATEWAY_POSIX)
    (void)agent_id;
    (void)message;
    (void)message_len;
    return SC_ERR_NOT_SUPPORTED;
#else
    pthread_mutex_lock(&pool->mu);
    sc_pool_slot_t *s = find_slot(pool, agent_id);
    if (!s) {
        pthread_mutex_unlock(&pool->mu);
        return SC_ERR_NOT_FOUND;
    }
    if (s->status != SC_AGENT_IDLE || !s->persistent_agent) {
        pthread_mutex_unlock(&pool->mu);
        return SC_ERR_INVALID_ARGUMENT;
    }
    s->status = SC_AGENT_RUNNING;
    sc_agent_t *ag = (sc_agent_t *)s->persistent_agent;
    pthread_mutex_unlock(&pool->mu);

    char *resp = NULL;
    size_t rlen = 0;
    sc_error_t err = sc_agent_turn(ag, message, message_len, &resp, &rlen);

    pthread_mutex_lock(&pool->mu);
    s->status = SC_AGENT_IDLE;
    if (err == SC_OK && resp && rlen > 0) {
        free_str(pool->alloc, &s->result);
        s->result = sc_strndup(pool->alloc, resp, rlen);
        *out_response = sc_strndup(pool->alloc, resp, rlen);
        *out_response_len = rlen;
    }
    if (resp)
        pool->alloc->free(pool->alloc->ctx, resp, rlen + 1);
    pthread_mutex_unlock(&pool->mu);
    return err;
#endif
#endif
}

sc_agent_status_t sc_agent_pool_status(sc_agent_pool_t *pool, uint64_t agent_id) {
    if (!pool)
        return SC_AGENT_FAILED;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    sc_pool_slot_t *s = find_slot(pool, agent_id);
    sc_agent_status_t st = s ? s->status : SC_AGENT_FAILED;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
    return st;
}

const char *sc_agent_pool_result(sc_agent_pool_t *pool, uint64_t agent_id) {
    if (!pool)
        return NULL;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    sc_pool_slot_t *s = find_slot(pool, agent_id);
    const char *r = s ? s->result : NULL;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
    return r;
}

sc_error_t sc_agent_pool_cancel(sc_agent_pool_t *pool, uint64_t agent_id) {
    if (!pool)
        return SC_ERR_INVALID_ARGUMENT;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    sc_pool_slot_t *s = find_slot(pool, agent_id);
    if (!s) {
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
        pthread_mutex_unlock(&pool->mu);
#endif
        return SC_ERR_NOT_FOUND;
    }
    s->cancelled = true;
    if (s->status == SC_AGENT_IDLE || s->status == SC_AGENT_COMPLETED) {
        if (pool->worktree_mgr) {
            sc_error_t rm_err = sc_worktree_remove(pool->worktree_mgr, s->agent_id);
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
            if (rm_err != SC_OK)
                fprintf(stderr, "[spawn] worktree cleanup failed: %s\n", sc_error_string(rm_err));
#else
            (void)rm_err;
#endif
        }
        slot_deinit_agent(pool->alloc, s);
        s->status = SC_AGENT_CANCELLED;
    }
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
    return SC_OK;
}

size_t sc_agent_pool_running_count(sc_agent_pool_t *pool) {
    if (!pool)
        return 0;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    size_t n = running_locked(pool);
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
    return n;
}

sc_error_t sc_agent_pool_list(sc_agent_pool_t *pool, sc_allocator_t *alloc,
                              sc_agent_pool_info_t **out, size_t *out_count) {
    if (!pool || !alloc || !out || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    size_t n = 0;
    for (uint32_t i = 0; i < SC_POOL_MAX_SLOTS; i++)
        if (pool->used[i])
            n++;
    if (n == 0)
        return SC_OK;
    sc_agent_pool_info_t *arr = (sc_agent_pool_info_t *)alloc->alloc(alloc->ctx, n * sizeof(*arr));
    if (!arr)
        return SC_ERR_OUT_OF_MEMORY;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    size_t j = 0;
    for (uint32_t i = 0; i < SC_POOL_MAX_SLOTS && j < n; i++) {
        if (!pool->used[i])
            continue;
        sc_pool_slot_t *s = &pool->slots[i];
        arr[j].agent_id = s->agent_id;
        arr[j].status = s->status;
        arr[j].label = s->label;
        arr[j].model = s->model;
        arr[j].mode = s->mode;
        arr[j].started_at = s->started_at;
        arr[j].cost_usd = s->cost_usd;
        j++;
    }
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
    *out = arr;
    *out_count = n;
    return SC_OK;
}
