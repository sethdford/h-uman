#ifndef SC_AGENT_SPAWN_H
#define SC_AGENT_SPAWN_H

#include "seaclaw/agent/mailbox.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/security.h"

typedef struct sc_worktree_manager sc_worktree_manager_t;
typedef struct sc_team_config sc_team_config_t;
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum sc_spawn_mode {
    SC_SPAWN_ONE_SHOT,
    SC_SPAWN_PERSISTENT,
} sc_spawn_mode_t;

typedef struct sc_spawn_config {
    const char *provider;
    size_t provider_len;
    const char *api_key;
    size_t api_key_len;
    const char *base_url;
    size_t base_url_len;
    const char *model;
    size_t model_len;
    double temperature;
    const char *workspace_dir;
    size_t workspace_dir_len;
    const char *system_prompt;
    size_t system_prompt_len;
    const char *const *tool_names;
    size_t tool_names_count;
    sc_security_policy_t *policy;
    double budget_usd;
    uint32_t max_iterations;
    sc_spawn_mode_t mode;
    sc_mailbox_t *mailbox; /* optional; when set, child agents share this mailbox */
} sc_spawn_config_t;

typedef enum sc_agent_status {
    SC_AGENT_RUNNING,
    SC_AGENT_IDLE,
    SC_AGENT_COMPLETED,
    SC_AGENT_FAILED,
    SC_AGENT_CANCELLED,
} sc_agent_status_t;

typedef struct sc_agent_pool sc_agent_pool_t;

sc_agent_pool_t *sc_agent_pool_create(sc_allocator_t *alloc, uint32_t max_concurrent);
void sc_agent_pool_destroy(sc_agent_pool_t *pool);

void sc_agent_pool_set_worktree_manager(sc_agent_pool_t *pool,
    sc_worktree_manager_t *worktree_mgr);

void sc_agent_pool_set_team_config(sc_agent_pool_t *pool, sc_team_config_t *team_config);

sc_error_t sc_agent_pool_spawn(sc_agent_pool_t *pool, const sc_spawn_config_t *cfg,
                               const char *task, size_t task_len, const char *label,
                               uint64_t *out_id);

sc_error_t sc_agent_pool_query(sc_agent_pool_t *pool, uint64_t agent_id, const char *message,
                               size_t message_len, char **out_response, size_t *out_response_len);

sc_agent_status_t sc_agent_pool_status(sc_agent_pool_t *pool, uint64_t agent_id);
const char *sc_agent_pool_result(sc_agent_pool_t *pool, uint64_t agent_id);
sc_error_t sc_agent_pool_cancel(sc_agent_pool_t *pool, uint64_t agent_id);
size_t sc_agent_pool_running_count(sc_agent_pool_t *pool);

typedef struct sc_agent_pool_info {
    uint64_t agent_id;
    sc_agent_status_t status;
    const char *label;
    const char *model;
    sc_spawn_mode_t mode;
    int64_t started_at;
    double cost_usd;
} sc_agent_pool_info_t;

sc_error_t sc_agent_pool_list(sc_agent_pool_t *pool, sc_allocator_t *alloc,
                              sc_agent_pool_info_t **out, size_t *out_count);

#endif /* SC_AGENT_SPAWN_H */
