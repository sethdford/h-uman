#include "human/core/log.h"
#include "human/agent/spawn.h"
#include "human/agent.h"
#include "human/agent/tool_context.h"
#include "human/cost.h"
#include "human/core/error.h"
#include "human/agent/mailbox.h"
#include "human/agent/team.h"
#include "human/agent/worktree.h"
#include "human/core/string.h"
#include "human/providers/factory.h"
#include "human/security.h"
#include "human/security/delegation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
#include <pthread.h>
#endif

#define HU_POOL_MAX_SLOTS    64
#define HU_POOL_DEFAULT_ITER 25

typedef struct hu_pool_slot {
    uint64_t agent_id;
    hu_agent_status_t status;
    hu_spawn_mode_t mode;
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
    hu_security_policy_t *policy;
    hu_mailbox_t *mailbox;
    /* Optional parent parity (not freed by slot; aliases caller-owned objects). */
    void *inherit_skillforge;
    const hu_tool_t *inherit_tools;
    size_t inherit_tools_count;
    void *inherit_memory;
    void *inherit_verifier_graph;
    void *inherit_session;
    void *inherit_observer;
    uint8_t inherit_autonomy;
    int64_t started_at;
    double cost_usd;
    uint32_t child_spawn_depth;
    hu_cost_tracker_t *inherit_cost_tracker;
    const hu_metacog_settings_t *inherit_metacognition_policy;
    void *parent_delegation_registry;
    uint64_t parent_agent_id;
    volatile bool cancelled;
    void *persistent_agent;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_t thread;
#endif
    bool thread_valid;
} hu_pool_slot_t;

struct hu_agent_pool {
    hu_allocator_t *alloc;
    hu_pool_slot_t slots[HU_POOL_MAX_SLOTS];
    bool used[HU_POOL_MAX_SLOTS];
    uint64_t next_id;
    uint32_t max_concurrent;
    hu_fleet_limits_t fleet_limits;
    uint64_t fleet_spawns_started;
    hu_cost_tracker_t *fleet_cost_tracker;
    hu_worktree_manager_t *worktree_mgr;
    hu_team_config_t *team_config;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_t mu;
#endif
};

static size_t running_locked(hu_agent_pool_t *p) {
    size_t n = 0;
    for (uint32_t i = 0; i < HU_POOL_MAX_SLOTS; i++)
        if (p->used[i] && p->slots[i].status == HU_AGENT_RUNNING)
            n++;
    return n;
}

static hu_pool_slot_t *find_slot(hu_agent_pool_t *p, uint64_t id) {
    for (uint32_t i = 0; i < HU_POOL_MAX_SLOTS; i++)
        if (p->used[i] && p->slots[i].agent_id == id)
            return &p->slots[i];
    return NULL;
}

static void free_str(hu_allocator_t *a, char **s) {
    if (*s) {
        a->free(a->ctx, *s, strlen(*s) + 1);
        *s = NULL;
    }
}

static void slot_free(hu_allocator_t *a, hu_pool_slot_t *s) {
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

static void slot_deinit_agent(hu_allocator_t *a, hu_pool_slot_t *s) {
    if (s->persistent_agent) {
        hu_agent_t *ag = (hu_agent_t *)s->persistent_agent;
        hu_agent_deinit(ag);
        a->free(a->ctx, ag, sizeof(hu_agent_t));
        s->persistent_agent = NULL;
    }
}

static char *dup_opt(hu_allocator_t *a, const char *s, size_t len) {
    if (!s || len == 0)
        return NULL;
    return hu_strndup(a, s, len);
}

#if defined(HU_GATEWAY_POSIX) && (!defined(HU_IS_TEST) || HU_IS_TEST == 0)
typedef struct {
    hu_agent_pool_t *pool;
    uint32_t slot;
} hu_spawn_tctx_t;

static void *spawn_thread(void *arg) {
    hu_spawn_tctx_t *tc = (hu_spawn_tctx_t *)arg;
    hu_agent_pool_t *pool = tc->pool;
    hu_pool_slot_t *s = &pool->slots[tc->slot];
    hu_allocator_t *a = pool->alloc;
    char *result = NULL;
    hu_provider_t prov = {0};
    bool created_prov = false;

    if (s->cancelled)
        goto done;

    const char *pn = s->provider_name ? s->provider_name : "openai";
    const char *ak = s->api_key ? s->api_key : "";
    const char *bu = s->base_url ? s->base_url : "";
    if (hu_provider_create(a, pn, strlen(pn), ak, strlen(ak), bu, strlen(bu), &prov) != HU_OK) {
        result = hu_strndup(a, "(provider create failed)", 24);
        goto done;
    }
    created_prov = true;

    const char *mdl = (s->model && s->model[0]) ? s->model : "gpt-4o-mini";
    double temp = s->temperature > 0.0 ? s->temperature : 0.7;
    const char *ws = (s->workspace_dir && s->workspace_dir[0]) ? s->workspace_dir : ".";
    uint32_t mi = s->max_iterations > 0 ? s->max_iterations : HU_POOL_DEFAULT_ITER;
    const char *sys = (s->system_prompt && s->system_prompt[0]) ? s->system_prompt
                                                                : "You are a helpful assistant.";
    const char *task = s->task ? s->task : "";
    size_t task_len = s->task ? strlen(s->task) : 0;

    if (s->mode == HU_SPAWN_PERSISTENT) {
        hu_agent_t *ag = (hu_agent_t *)a->alloc(a->ctx, sizeof(hu_agent_t));
        if (!ag) {
            result = hu_strndup(a, "(oom)", 5);
            goto done;
        }
        memset(ag, 0, sizeof(*ag));
        if (hu_agent_from_config(ag, a, prov, s->inherit_tools, s->inherit_tools_count,
                                 (hu_memory_t *)s->inherit_memory,
                                 (hu_session_store_t *)s->inherit_session,
                                 (hu_observer_t *)s->inherit_observer, s->policy, mdl,
                                 strlen(mdl), pn, strlen(pn), temp, ws, strlen(ws), mi, 50, false,
                                 s->inherit_autonomy, sys, strlen(sys), s->persona_name,
                                 s->persona_name ? strlen(s->persona_name) : 0, NULL) != HU_OK) {
            a->free(a->ctx, ag, sizeof(*ag));
            result = hu_strndup(a, "(agent create failed)", 21);
            goto done;
        }
#ifdef HU_ENABLE_SQLITE
        if (s->inherit_verifier_graph) {
            hu_error_t ge = hu_agent_bind_sqlite_graph(
                ag, (struct hu_graph *)s->inherit_verifier_graph, a);
            if (ge != HU_OK)
                hu_log_warn("spawn", NULL, "spawn graph bind: %s", hu_error_string(ge));
        }
#endif
        if (s->inherit_metacognition_policy)
            hu_metacognition_apply_config(&ag->infra.metacognition, s->inherit_metacognition_policy);
        ag->agent_id = s->agent_id;
        ag->spawn_depth = s->child_spawn_depth;
        ag->worktree_mgr = pool->worktree_mgr;
        ag->agent_pool = pool;
#ifdef HU_HAS_SKILLS
        if (s->inherit_skillforge)
            hu_agent_set_skillforge(ag, (struct hu_skillforge *)s->inherit_skillforge);
#endif
        if (s->inherit_cost_tracker)
            hu_agent_set_cost_tracker(ag, s->inherit_cost_tracker);
        if (s->mailbox)
            hu_agent_set_mailbox(ag, s->mailbox);

        /* Issue delegation token from parent to child if parent has delegation registry */
        if (s->parent_delegation_registry) {
            hu_delegation_registry_t *parent_reg = (hu_delegation_registry_t *)s->parent_delegation_registry;
            char parent_id_str[64];
            snprintf(parent_id_str, sizeof(parent_id_str), "%llu", (unsigned long long)s->parent_agent_id);
            char child_id_str[64];
            snprintf(child_id_str, sizeof(child_id_str), "%llu", (unsigned long long)s->agent_id);

            /* Build tool caveats from parent's tool list if available */
            hu_delegation_caveat_t caveat = {0};
            caveat.key = "tool";
            caveat.key_len = 4;
            caveat.value = "*"; /* unrestricted tool access by default */
            caveat.value_len = 1;

            const char *token_id = hu_delegation_issue(parent_reg, a, parent_id_str, child_id_str,
                                                      3600, &caveat, 1);
            if (token_id && strlen(token_id) < 64) {
                strncpy(ag->delegation_token_id, token_id, 63);
                ag->delegation_token_id[63] = '\0';
            }
        }

        char *resp = NULL;
        size_t rlen = 0;
        hu_error_t e = hu_agent_turn(ag, task, task_len, &resp, &rlen);
        if (e == HU_OK && resp && rlen > 0) {
            result = hu_strndup(a, resp, rlen);
            a->free(a->ctx, resp, rlen + 1);
        } else if (resp) {
            a->free(a->ctx, resp, rlen + 1);
        }
        if (!result)
            result = hu_strndup(a, "(no response)", 13);

        pthread_mutex_lock(&pool->mu);
        if (s->cancelled) {
            hu_agent_deinit(ag);
            a->free(a->ctx, ag, sizeof(*ag));
            s->status = HU_AGENT_CANCELLED;
        } else {
            s->persistent_agent = ag;
            s->result = result;
            result = NULL;
            s->status = HU_AGENT_IDLE;
        }
        pthread_mutex_unlock(&pool->mu);
    } else {
        hu_agent_t ag = {0};
        if (hu_agent_from_config(&ag, a, prov, s->inherit_tools, s->inherit_tools_count,
                                 (hu_memory_t *)s->inherit_memory,
                                 (hu_session_store_t *)s->inherit_session,
                                 (hu_observer_t *)s->inherit_observer, s->policy, mdl,
                                 strlen(mdl), pn, strlen(pn), temp, ws, strlen(ws), mi, 50, false,
                                 s->inherit_autonomy, sys, strlen(sys), s->persona_name,
                                 s->persona_name ? strlen(s->persona_name) : 0, NULL) != HU_OK) {
            result = hu_strndup(a, "(agent create failed)", 21);
            goto done;
        }
#ifdef HU_ENABLE_SQLITE
        if (s->inherit_verifier_graph) {
            hu_error_t ge = hu_agent_bind_sqlite_graph(
                &ag, (struct hu_graph *)s->inherit_verifier_graph, a);
            if (ge != HU_OK)
                hu_log_warn("spawn", NULL, "spawn graph bind: %s", hu_error_string(ge));
        }
#endif
        if (s->inherit_metacognition_policy)
            hu_metacognition_apply_config(&ag.infra.metacognition, s->inherit_metacognition_policy);
        ag.agent_id = s->agent_id;
        ag.spawn_depth = s->child_spawn_depth;
        ag.worktree_mgr = pool->worktree_mgr;
        ag.agent_pool = pool;
#ifdef HU_HAS_SKILLS
        if (s->inherit_skillforge)
            hu_agent_set_skillforge(&ag, (struct hu_skillforge *)s->inherit_skillforge);
#endif
        if (s->inherit_cost_tracker)
            hu_agent_set_cost_tracker(&ag, s->inherit_cost_tracker);
        if (s->mailbox)
            hu_agent_set_mailbox(&ag, s->mailbox);

        /* Issue delegation token from parent to child if parent has delegation registry */
        if (s->parent_delegation_registry) {
            hu_delegation_registry_t *parent_reg = (hu_delegation_registry_t *)s->parent_delegation_registry;
            char parent_id_str[64];
            snprintf(parent_id_str, sizeof(parent_id_str), "%llu", (unsigned long long)s->parent_agent_id);
            char child_id_str[64];
            snprintf(child_id_str, sizeof(child_id_str), "%llu", (unsigned long long)s->agent_id);

            hu_delegation_caveat_t caveat = {0};
            caveat.key = "tool";
            caveat.key_len = 4;
            caveat.value = "*";
            caveat.value_len = 1;

            const char *token_id = hu_delegation_issue(parent_reg, a, parent_id_str, child_id_str,
                                                      3600, &caveat, 1);
            if (token_id && strlen(token_id) < 64) {
                strncpy(ag.delegation_token_id, token_id, 63);
                ag.delegation_token_id[63] = '\0';
            }
        }

        char *resp = NULL;
        size_t rlen = 0;
        hu_error_t turn_err = hu_agent_turn(&ag, task, task_len, &resp, &rlen);
        if (resp && rlen > 0) {
            result = hu_strndup(a, resp, rlen);
            a->free(a->ctx, resp, rlen + 1);
        } else if (resp) {
            a->free(a->ctx, resp, rlen + 1);
        }
        hu_agent_deinit(&ag);
        if (!result) {
            const char *err_str = hu_error_string(turn_err);
            result = hu_strndup(a, err_str, strlen(err_str));
        }
        if (pool->worktree_mgr) {
            hu_error_t rm_err = hu_worktree_remove(pool->worktree_mgr, s->agent_id);
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
            if (rm_err != HU_OK)
                hu_log_error("spawn", NULL, "worktree cleanup failed: %s", hu_error_string(rm_err));
#else
            (void)rm_err;
#endif
        }
    }

done:
    if (created_prov && prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, a);

    pthread_mutex_lock(&pool->mu);
    if (s->status != HU_AGENT_IDLE && s->status != HU_AGENT_CANCELLED) {
        s->status = s->cancelled ? HU_AGENT_CANCELLED : HU_AGENT_COMPLETED;
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

hu_agent_pool_t *hu_agent_pool_create(hu_allocator_t *alloc, uint32_t max_concurrent) {
    if (!alloc)
        return NULL;
    hu_agent_pool_t *p = (hu_agent_pool_t *)alloc->alloc(alloc->ctx, sizeof(*p));
    if (!p)
        return NULL;
    memset(p, 0, sizeof(*p));
    p->alloc = alloc;
    p->next_id = 1;
    p->max_concurrent = max_concurrent > 0 ? max_concurrent : 4;
    p->fleet_limits.max_spawn_depth = 8;
    p->fleet_limits.max_total_spawns = 0;
    p->fleet_limits.budget_limit_usd = 0.0;
    p->fleet_spawns_started = 0;
    p->fleet_cost_tracker = NULL;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    if (pthread_mutex_init(&p->mu, NULL) != 0) {
        alloc->free(alloc->ctx, p, sizeof(*p));
        return NULL;
    }
#endif
    return p;
}

void hu_agent_pool_set_worktree_manager(hu_agent_pool_t *pool,
                                        hu_worktree_manager_t *worktree_mgr) {
    if (!pool)
        return;
    pool->worktree_mgr = worktree_mgr;
}

void hu_agent_pool_set_team_config(hu_agent_pool_t *pool, hu_team_config_t *team_config) {
    if (!pool)
        return;
    pool->team_config = team_config;
}

void hu_agent_pool_set_fleet_limits(hu_agent_pool_t *pool, const hu_fleet_limits_t *limits) {
    if (!pool)
        return;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    if (!limits) {
        pool->fleet_limits.max_spawn_depth = 8;
        pool->fleet_limits.max_total_spawns = 0;
        pool->fleet_limits.budget_limit_usd = 0.0;
    } else {
        pool->fleet_limits = *limits;
    }
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
}

void hu_agent_pool_bind_fleet_cost_tracker(hu_agent_pool_t *pool, hu_cost_tracker_t *tracker) {
    if (!pool)
        return;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    pool->fleet_cost_tracker = tracker;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
}

void hu_agent_pool_fleet_status(hu_agent_pool_t *pool, hu_fleet_status_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!pool)
        return;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    out->limits = pool->fleet_limits;
    out->spawns_started = pool->fleet_spawns_started;
    out->running = running_locked(pool);
    for (uint32_t i = 0; i < HU_POOL_MAX_SLOTS; i++)
        if (pool->used[i])
            out->slots_used++;
    if (pool->fleet_cost_tracker)
        out->session_spend_usd = hu_cost_session_total(pool->fleet_cost_tracker);
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
}

void hu_agent_pool_destroy(hu_agent_pool_t *pool) {
    if (!pool)
        return;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    for (uint32_t i = 0; i < HU_POOL_MAX_SLOTS; i++) {
        if (!pool->used[i])
            continue;
        hu_pool_slot_t *s = &pool->slots[i];
        if (pool->worktree_mgr && s->agent_id) {
            hu_error_t rm_err = hu_worktree_remove(pool->worktree_mgr, s->agent_id);
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
            if (rm_err != HU_OK)
                hu_log_error("spawn", NULL, "worktree cleanup failed: %s", hu_error_string(rm_err));
#else
            (void)rm_err;
#endif
        }
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
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
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
    pthread_mutex_destroy(&pool->mu);
#endif
    pool->alloc->free(pool->alloc->ctx, pool, sizeof(*pool));
}

hu_error_t hu_agent_pool_spawn(hu_agent_pool_t *pool, const hu_spawn_config_t *cfg,
                               const char *task, size_t task_len, const char *label,
                               uint64_t *out_id) {
    if (!pool || !cfg || !out_id)
        return HU_ERR_INVALID_ARGUMENT;
    hu_allocator_t *a = pool->alloc;

#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    {
        uint32_t child_depth = cfg->caller_spawn_depth + 1u;
        if (pool->fleet_limits.max_spawn_depth > 0u &&
            child_depth > pool->fleet_limits.max_spawn_depth) {
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
            pthread_mutex_unlock(&pool->mu);
#endif
            return HU_ERR_FLEET_DEPTH_EXCEEDED;
        }
        if (pool->fleet_limits.max_total_spawns > 0u &&
            pool->fleet_spawns_started >= pool->fleet_limits.max_total_spawns) {
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
            pthread_mutex_unlock(&pool->mu);
#endif
            return HU_ERR_FLEET_SPAWN_CAP;
        }
        hu_cost_tracker_t *acct = cfg->shared_cost_tracker ? cfg->shared_cost_tracker
                                                           : pool->fleet_cost_tracker;
        if (pool->fleet_limits.budget_limit_usd > 0.0) {
            if (!acct) {
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
                pthread_mutex_unlock(&pool->mu);
#endif
                return HU_ERR_INVALID_ARGUMENT;
            }
            if (hu_cost_session_total(acct) >= pool->fleet_limits.budget_limit_usd) {
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
                pthread_mutex_unlock(&pool->mu);
#endif
                return HU_ERR_FLEET_BUDGET_EXCEEDED;
            }
        }
    }
    if (running_locked(pool) >= pool->max_concurrent) {
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
        pthread_mutex_unlock(&pool->mu);
#endif
        return HU_ERR_SUBAGENT_TOO_MANY;
    }
    uint32_t si = HU_POOL_MAX_SLOTS;
    for (uint32_t i = 0; i < HU_POOL_MAX_SLOTS; i++)
        if (!pool->used[i]) {
            si = i;
            break;
        }
    if (si >= HU_POOL_MAX_SLOTS) {
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
        pthread_mutex_unlock(&pool->mu);
#endif
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_pool_slot_t *s = &pool->slots[si];
    memset(s, 0, sizeof(*s));
    s->agent_id = pool->next_id++;
    s->status = HU_AGENT_RUNNING;
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
        if (hu_worktree_create(pool->worktree_mgr, s->agent_id, lbl, &wt_path) == HU_OK &&
            wt_path) {
            s->workspace_dir = hu_strdup(a, wt_path);
        } else {
            s->workspace_dir = dup_opt(a, cfg->workspace_dir, cfg->workspace_dir_len);
        }
    } else {
        s->workspace_dir = dup_opt(a, cfg->workspace_dir, cfg->workspace_dir_len);
    }
    s->system_prompt = dup_opt(a, cfg->system_prompt, cfg->system_prompt_len);
    s->persona_name = dup_opt(a, cfg->persona_name, cfg->persona_name_len);
    s->task = task ? hu_strndup(a, task, task_len) : NULL;
    s->label = label ? hu_strndup(a, label, strlen(label)) : NULL;
    s->mailbox = cfg->mailbox;
    s->inherit_skillforge = cfg->skillforge;
    s->inherit_tools = (const hu_tool_t *)cfg->parent_tools;
    s->inherit_tools_count = cfg->parent_tools_count;
    s->inherit_memory = cfg->memory;
    s->inherit_verifier_graph = cfg->verifier_graph;
    s->inherit_session = cfg->session_store;
    s->inherit_observer = cfg->observer;
    s->inherit_autonomy = cfg->autonomy_level > 0 ? cfg->autonomy_level : 2;
    s->child_spawn_depth = cfg->caller_spawn_depth + 1u;
    s->inherit_cost_tracker = cfg->shared_cost_tracker ? cfg->shared_cost_tracker
                                                       : pool->fleet_cost_tracker;
    s->inherit_metacognition_policy = cfg->metacognition_policy;
    s->parent_delegation_registry = cfg->parent_delegation_registry;
    s->parent_agent_id = cfg->parent_agent_id;
    pool->used[si] = true;
    pool->fleet_spawns_started++;

#if defined(HU_IS_TEST) && HU_IS_TEST == 1
    s->result = hu_sprintf(a, "(spawned: %s)", s->label ? s->label : "agent");
    if (!s->result)
        s->result = hu_strndup(a, "(spawned)", 9);
    s->status = (cfg->mode == HU_SPAWN_PERSISTENT) ? HU_AGENT_IDLE : HU_AGENT_COMPLETED;
    if (pool->worktree_mgr && s->status == HU_AGENT_COMPLETED) {
        hu_error_t rm_err = hu_worktree_remove(pool->worktree_mgr, s->agent_id);
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
        if (rm_err != HU_OK)
            hu_log_error("spawn", NULL, "worktree cleanup failed: %s", hu_error_string(rm_err));
#else
        (void)rm_err;
#endif
    }
#elif defined(HU_GATEWAY_POSIX)
    {
        hu_spawn_tctx_t *tc = (hu_spawn_tctx_t *)a->alloc(a->ctx, sizeof(*tc));
        if (!tc) {
            slot_free(a, s);
            pool->used[si] = false;
            pthread_mutex_unlock(&pool->mu);
            return HU_ERR_OUT_OF_MEMORY;
        }
        tc->pool = pool;
        tc->slot = si;
        if (pthread_create(&s->thread, NULL, spawn_thread, tc) != 0) {
            a->free(a->ctx, tc, sizeof(*tc));
            slot_free(a, s);
            pool->used[si] = false;
            pthread_mutex_unlock(&pool->mu);
            return HU_ERR_INTERNAL;
        }
        s->thread_valid = true;
    }
    pthread_mutex_unlock(&pool->mu);
#else
    s->result = hu_sprintf(a, "(spawned: %s)", s->label ? s->label : "agent");
    if (!s->result)
        s->result = hu_strndup(a, "(spawned)", 9);
    s->status = (cfg->mode == HU_SPAWN_PERSISTENT) ? HU_AGENT_IDLE : HU_AGENT_COMPLETED;
#endif

    *out_id = s->agent_id;
    return HU_OK;
}

hu_error_t hu_agent_pool_query(hu_agent_pool_t *pool, uint64_t agent_id, const char *message,
                               size_t message_len, char **out_response, size_t *out_response_len) {
    if (!pool || !out_response || !out_response_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_response = NULL;
    *out_response_len = 0;

#if defined(HU_IS_TEST) && HU_IS_TEST == 1
    (void)agent_id;
    (void)message;
    (void)message_len;
    *out_response = hu_strndup(pool->alloc, "(query response)", 16);
    *out_response_len = 16;
    return HU_OK;
#else
#if !defined(HU_GATEWAY_POSIX)
    (void)agent_id;
    (void)message;
    (void)message_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    pthread_mutex_lock(&pool->mu);
    hu_pool_slot_t *s = find_slot(pool, agent_id);
    if (!s) {
        pthread_mutex_unlock(&pool->mu);
        return HU_ERR_NOT_FOUND;
    }
    if (s->status != HU_AGENT_IDLE || !s->persistent_agent) {
        pthread_mutex_unlock(&pool->mu);
        return HU_ERR_INVALID_ARGUMENT;
    }
    s->status = HU_AGENT_RUNNING;
    hu_agent_t *ag = (hu_agent_t *)s->persistent_agent;
    pthread_mutex_unlock(&pool->mu);

    char *resp = NULL;
    size_t rlen = 0;
    hu_error_t err = hu_agent_turn(ag, message, message_len, &resp, &rlen);

    pthread_mutex_lock(&pool->mu);
    s->status = HU_AGENT_IDLE;
    if (err == HU_OK && resp && rlen > 0) {
        free_str(pool->alloc, &s->result);
        s->result = hu_strndup(pool->alloc, resp, rlen);
        *out_response = hu_strndup(pool->alloc, resp, rlen);
        *out_response_len = rlen;
    }
    if (resp)
        pool->alloc->free(pool->alloc->ctx, resp, rlen + 1);
    pthread_mutex_unlock(&pool->mu);
    return err;
#endif
#endif
}

hu_agent_status_t hu_agent_pool_status(hu_agent_pool_t *pool, uint64_t agent_id) {
    if (!pool)
        return HU_AGENT_FAILED;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    hu_pool_slot_t *s = find_slot(pool, agent_id);
    hu_agent_status_t st = s ? s->status : HU_AGENT_FAILED;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
    return st;
}

const char *hu_agent_pool_result(hu_agent_pool_t *pool, uint64_t agent_id) {
    if (!pool)
        return NULL;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    hu_pool_slot_t *s = find_slot(pool, agent_id);
    const char *r = s ? s->result : NULL;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
    return r;
}

hu_error_t hu_agent_pool_cancel(hu_agent_pool_t *pool, uint64_t agent_id) {
    if (!pool)
        return HU_ERR_INVALID_ARGUMENT;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    hu_pool_slot_t *s = find_slot(pool, agent_id);
    if (!s) {
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
        pthread_mutex_unlock(&pool->mu);
#endif
        return HU_ERR_NOT_FOUND;
    }
    s->cancelled = true;
    if (s->status == HU_AGENT_IDLE || s->status == HU_AGENT_COMPLETED) {
        if (pool->worktree_mgr) {
            hu_error_t rm_err = hu_worktree_remove(pool->worktree_mgr, s->agent_id);
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
            if (rm_err != HU_OK)
                hu_log_error("spawn", NULL, "worktree cleanup failed: %s", hu_error_string(rm_err));
#else
            (void)rm_err;
#endif
        }
        slot_deinit_agent(pool->alloc, s);
        s->status = HU_AGENT_CANCELLED;
    }
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
    return HU_OK;
}

size_t hu_agent_pool_running_count(hu_agent_pool_t *pool) {
    if (!pool)
        return 0;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    size_t n = running_locked(pool);
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
    return n;
}

hu_error_t hu_agent_pool_list(hu_agent_pool_t *pool, hu_allocator_t *alloc,
                              hu_agent_pool_info_t **out, size_t *out_count) {
    if (!pool || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    size_t n = 0;
    for (uint32_t i = 0; i < HU_POOL_MAX_SLOTS; i++)
        if (pool->used[i])
            n++;
    if (n == 0)
        return HU_OK;
    hu_agent_pool_info_t *arr = (hu_agent_pool_info_t *)alloc->alloc(alloc->ctx, n * sizeof(*arr));
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_lock(&pool->mu);
#endif
    size_t j = 0;
    for (uint32_t i = 0; i < HU_POOL_MAX_SLOTS && j < n; i++) {
        if (!pool->used[i])
            continue;
        hu_pool_slot_t *s = &pool->slots[i];
        arr[j].agent_id = s->agent_id;
        arr[j].status = s->status;
        arr[j].label = s->label;
        arr[j].model = s->model;
        arr[j].mode = s->mode;
        arr[j].started_at = s->started_at;
        arr[j].cost_usd = s->cost_usd;
        j++;
    }
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_unlock(&pool->mu);
#endif
    *out = arr;
    *out_count = n;
    return HU_OK;
}

/* ─── Named agent spawn ──────────────────────────────────────────────────── */

#include "human/agent/registry.h"
#include "human/config_types.h"

void hu_spawn_config_from_named(hu_spawn_config_t *out, const hu_named_agent_config_t *cfg) {
    if (!out || !cfg)
        return;
    memset(out, 0, sizeof(*out));
    out->provider = cfg->provider;
    out->provider_len = cfg->provider ? strlen(cfg->provider) : 0;
    out->model = cfg->model;
    out->model_len = cfg->model ? strlen(cfg->model) : 0;
    out->temperature = cfg->temperature;
    out->system_prompt = cfg->system_prompt;
    out->system_prompt_len = cfg->system_prompt ? strlen(cfg->system_prompt) : 0;
    out->persona_name = cfg->persona;
    out->persona_name_len = cfg->persona ? strlen(cfg->persona) : 0;
    out->tool_names = cfg->enabled_tools;
    out->tool_names_count = cfg->enabled_tools_count;
    out->budget_usd = cfg->budget_usd;
    out->max_iterations = cfg->max_iterations;
    out->mode = HU_SPAWN_ONE_SHOT;
}

void hu_spawn_config_apply_current_tool_agent(hu_spawn_config_t *cfg) {
    if (!cfg)
        return;
    hu_agent_t *cur = hu_agent_get_current_for_tools();
    if (!cur)
        return;
    cfg->caller_spawn_depth = cur->spawn_depth;
    cfg->shared_cost_tracker = cur->cost_tracker;
    cfg->metacognition_policy = &cur->infra.metacognition.cfg;
}

void hu_spawn_config_apply_parent_agent(hu_spawn_config_t *cfg, const hu_agent_t *parent) {
    if (!cfg || !parent)
        return;

    if (parent->persona_name && parent->persona_name_len > 0) {
        cfg->persona_name = parent->persona_name;
        cfg->persona_name_len = parent->persona_name_len;
    }
#ifdef HU_HAS_SKILLS
    if (parent->skillforge)
        cfg->skillforge = parent->skillforge;
#endif
    if (parent->tools && parent->tools_count > 0) {
        cfg->parent_tools = (const void *)parent->tools;
        cfg->parent_tools_count = parent->tools_count;
    }
    if (parent->memory)
        cfg->memory = parent->memory;
    if (parent->verifier_graph)
        cfg->verifier_graph = parent->verifier_graph;
    if (parent->session_store)
        cfg->session_store = parent->session_store;
    if (parent->observer && parent->observer->vtable)
        cfg->observer = parent->observer;
    if (parent->policy)
        cfg->policy = parent->policy;
    cfg->autonomy_level = parent->autonomy_level;
    cfg->caller_spawn_depth = parent->spawn_depth;
    cfg->shared_cost_tracker = parent->cost_tracker;
    cfg->metacognition_policy = &parent->infra.metacognition.cfg;
    cfg->parent_delegation_registry = (void *)parent->infra.delegation_registry;
    cfg->parent_agent_id = parent->agent_id;

    if (parent->turn_model && parent->turn_model_len > 0) {
        cfg->model = parent->turn_model;
        cfg->model_len = parent->turn_model_len;
    } else if (parent->model_name && parent->model_name_len > 0) {
        cfg->model = parent->model_name;
        cfg->model_len = parent->model_name_len;
    }
    cfg->temperature = parent->temperature;
    if (parent->workspace_dir && parent->workspace_dir_len > 0) {
        cfg->workspace_dir = parent->workspace_dir;
        cfg->workspace_dir_len = parent->workspace_dir_len;
    }
    if (parent->default_provider && parent->default_provider_len > 0) {
        cfg->provider = parent->default_provider;
        cfg->provider_len = parent->default_provider_len;
    } else if (parent->provider.vtable && parent->provider.vtable->get_name) {
        const char *pn = parent->provider.vtable->get_name(parent->provider.ctx);
        if (pn) {
            cfg->provider = pn;
            cfg->provider_len = strlen(pn);
        }
    }
    cfg->mailbox = parent->mailbox;
    cfg->mode = HU_SPAWN_ONE_SHOT;
    if (parent->max_tool_iterations > 0)
        cfg->max_iterations = parent->max_tool_iterations;
}

hu_error_t hu_agent_pool_spawn_named(hu_agent_pool_t *pool, const hu_agent_registry_t *registry,
                                     const char *agent_name, const char *task, size_t task_len,
                                     uint64_t *out_id) {
    if (!pool || !registry || !agent_name || !task)
        return HU_ERR_INVALID_ARGUMENT;

    const hu_named_agent_config_t *cfg = hu_agent_registry_get(registry, agent_name);
    if (!cfg)
        return HU_ERR_NOT_FOUND;

    hu_spawn_config_t spawn_cfg;
    hu_spawn_config_from_named(&spawn_cfg, cfg);
    hu_spawn_config_apply_current_tool_agent(&spawn_cfg);
    return hu_agent_pool_spawn(pool, &spawn_cfg, task, task_len, agent_name, out_id);
}
