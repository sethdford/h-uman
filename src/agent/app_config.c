#include "human/agent/app_config.h"
#include <string.h>

hu_agent_app_config_t hu_agent_app_config_from(const hu_config_t *cfg) {
    hu_agent_app_config_t app;
    memset(&app, 0, sizeof(app));

    if (!cfg)
        return app;

    /* Agent initialization fields */
    app.default_provider = cfg->default_provider;
    app.default_provider_len = cfg->default_provider ? strlen(cfg->default_provider) : 0;
    app.default_model = cfg->default_model;
    app.default_model_len = cfg->default_model ? strlen(cfg->default_model) : 0;
    app.temperature = cfg->temperature;
    app.workspace_dir = cfg->workspace_dir;
    app.workspace_dir_len = cfg->workspace_dir ? strlen(cfg->workspace_dir) : 0;
    app.max_tool_iterations = cfg->agent.max_tool_iterations;
    app.max_history_messages = cfg->agent.max_history_messages;
    app.auto_save = cfg->memory_auto_save;
    app.autonomy_level = cfg->security.autonomy_level;
    app.persona = cfg->agent.persona;
    app.persona_len = cfg->agent.persona ? strlen(cfg->agent.persona) : 0;

    /* Context pressure + agent feature gates (from hu_agent_context_config_t) */
    app.token_limit = cfg->agent.token_limit;
    app.pressure_warn = cfg->agent.context_pressure_warn;
    app.pressure_compact = cfg->agent.context_pressure_compact;
    app.compact_target = cfg->agent.context_compact_target;
    app.compact_context = cfg->agent.compact_context;
    app.llm_compiler_enabled = cfg->agent.llm_compiler_enabled;
    app.mcts_planner_enabled = cfg->agent.mcts_planner_enabled;
    app.tree_of_thought = cfg->agent.tree_of_thought;
    app.constitutional_ai = cfg->agent.constitutional_ai;
    app.constitutional_style_rules_enabled = cfg->agent.constitutional_style_rules_enabled;
    app.speculative_cache = cfg->agent.speculative_cache;
    app.tool_routing_enabled = cfg->agent.tool_routing_enabled;
    app.multi_agent = cfg->agent.multi_agent;
    app.hula_enabled = cfg->agent.hula_enabled;
    app.compaction_use_structured = cfg->agent.compaction_use_structured;

    /* Top agent-core readers: tools, provider, model (NOTE: provider and tools
     * are not yet in this projection since they're passed as separate parameters
     * to hu_agent_from_config. Model projection is the default_model above.
     * These placeholder fields stay NULL/0 for now; future migrations will wire
     * them as the provider becomes injected. */
    app.tools = NULL;
    app.tools_count = 0;
    memset(&app.provider, 0, sizeof(app.provider));
    app.model = cfg->default_model;
    app.model_len = cfg->default_model ? strlen(cfg->default_model) : 0;

    return app;
}
