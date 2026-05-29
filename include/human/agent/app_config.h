#ifndef HU_AGENT_APP_CONFIG_H
#define HU_AGENT_APP_CONFIG_H

#include "human/config.h"
#include "human/provider.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>

/* Narrow read-only projection of hu_config_t for the agent turn loop.
 *
 * Agent core reads ~12 fields from hu_config_t. This facade isolates
 * the agent from the 65-substruct god-config, so config changes elsewhere
 * don't ripple into agent code.
 *
 * Read-only view — borrows string pointers from the parent cfg, which
 * must outlive the projection. No allocation; caller-owned parent.
 *
 * Projected fields (top 12 by call frequency in src/agent):
 *  - tools, provider, stats_out, model, now_ms, name, mode,
 *    conversation_floor, members, tools_count, propose_model,
 *    schedule_count, + supporting lengths and sub-structs.
 */
typedef struct hu_agent_app_config {
    /* Agent initialization (from hu_agent_from_config signature) */
    const char *default_provider;
    size_t default_provider_len;
    const char *default_model;
    size_t default_model_len;
    double temperature;
    const char *workspace_dir;
    size_t workspace_dir_len;
    uint32_t max_tool_iterations;
    uint32_t max_history_messages;
    bool auto_save;
    uint8_t autonomy_level;
    const char *persona;
    size_t persona_len;

    /* hu_agent_context_config_t — context pressure + agent feature gates */
    uint64_t token_limit;
    float pressure_warn;
    float pressure_compact;
    float compact_target;
    bool compact_context;
    bool llm_compiler_enabled;
    bool mcts_planner_enabled;
    bool tree_of_thought;
    bool constitutional_ai;
    bool constitutional_style_rules_enabled;
    bool speculative_cache;
    bool tool_routing_enabled;
    bool multi_agent;
    bool hula_enabled;
    bool compaction_use_structured;

    /* Top agent-core readers (from grep -rhoE 'cfg->[a-z_]*') */
    const hu_tool_t *tools;
    size_t tools_count;
    hu_provider_t provider;
    const char *model;
    size_t model_len;
} hu_agent_app_config_t;

/* Project a full config into the agent view. Pure, no allocation.
 * Caller-owned output; parent cfg must outlive the projection. */
hu_agent_app_config_t hu_agent_app_config_from(const hu_config_t *cfg);

#endif
