#include "human/agent.h"
#include "human/agent/awareness.h"
#include "human/agent/commitment_store.h"
#include "human/agent/pattern_radar.h"
#include "human/agent/superhuman.h"
#include "human/agent/superhuman_commitment.h"
#include "human/agent/superhuman_emotional.h"
#include "human/agent/superhuman_predictive.h"
#include "human/agent/superhuman_silence.h"
#include "human/memory/consolidation.h"
#include "human/memory/promotion.h"
#ifdef HU_HAS_PERSONA
#include "human/persona/circadian.h"
#include "human/persona/relationship.h"
#endif
#include "human/agent/commands.h"
#include "human/agent/compaction.h"
#include "human/agent/dispatcher.h"
#include "human/agent/episodic.h"
#include "human/agent/input_guard.h"
#include "human/agent/mailbox.h"
#include "human/agent/memory_loader.h"
#include "human/agent/outcomes.h"
#include "human/agent/planner.h"
#include "human/agent/preferences.h"
#include "human/agent/prompt.h"
#include "human/agent/speculative.h"
#include "human/agent/reflection.h"
#include "human/agent/task_list.h"
#include "human/agent/team.h"
#include "human/agent/tool_context.h"
#include "human/agent/undo.h"
#include "human/context.h"
#include "human/context_tokens.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory/stm.h"
#include "human/observer.h"
#ifdef HU_HAS_PERSONA
#include "human/persona.h"
#endif
#include "human/provider.h"
#include "human/voice.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void hu_agent_internal_generate_trace_id(char *buf) {
    static uint32_t counter = 0;
    uint64_t t = (uint64_t)clock();
    counter++;
    snprintf(buf, 37, "%08x-%04x-%04x-%04x-%08x%04x", (uint32_t)(t & 0xFFFFFFFF),
             (uint16_t)((t >> 32) & 0xFFFF), (uint16_t)(0x4000 | (counter & 0x0FFF)),
             (uint16_t)(0x8000 | ((t >> 16) & 0x3FFF)), (uint32_t)(t * 2654435761u),
             (uint16_t)(counter & 0xFFFF));
}

uint64_t hu_agent_internal_clock_diff_ms(clock_t start, clock_t end) {
    return (uint64_t)((end - start) * 1000 / CLOCKS_PER_SEC);
}

static _Thread_local hu_agent_t *hu__current_agent_for_tools;

void hu_agent_set_current_for_tools(hu_agent_t *agent) {
    hu__current_agent_for_tools = agent;
}
void hu_agent_clear_current_for_tools(void) {
    hu__current_agent_for_tools = NULL;
}
hu_agent_t *hu_agent_get_current_for_tools(void) {
    return hu__current_agent_for_tools;
}

void hu_agent_internal_record_cost(hu_agent_t *agent, const hu_token_usage_t *usage) {
    if (!agent->cost_tracker || !agent->cost_tracker->enabled)
        return;
    hu_cost_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.model = agent->model_name;
    entry.input_tokens = usage->prompt_tokens;
    entry.output_tokens = usage->completion_tokens;
    entry.total_tokens = usage->total_tokens;
    entry.cost_usd = 0.0;
    entry.timestamp_secs = (int64_t)time(NULL);
    (void)hu_cost_record_usage(agent->cost_tracker, &entry, agent->active_job_id);
}

#define HU_AGENT_HISTORY_INIT_CAP 16
#define HU_AGENT_MAX_SLASH_LEN    256

hu_error_t hu_agent_from_config(
    hu_agent_t *out, hu_allocator_t *alloc, hu_provider_t provider, const hu_tool_t *tools,
    size_t tools_count, hu_memory_t *memory, hu_session_store_t *session_store,
    hu_observer_t *observer, hu_security_policy_t *policy, const char *model_name,
    size_t model_name_len, const char *default_provider, size_t default_provider_len,
    double temperature, const char *workspace_dir, size_t workspace_dir_len,
    uint32_t max_tool_iterations, uint32_t max_history_messages, bool auto_save,
    uint8_t autonomy_level, const char *custom_instructions, size_t custom_instructions_len,
    const char *persona, size_t persona_len, const hu_agent_context_config_t *ctx_cfg) {
    if (!out || !alloc || !provider.vtable || !model_name) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));

    out->alloc = alloc;
    out->provider = provider;
    out->memory = memory;
    out->retrieval_engine = NULL;
    out->session_store = session_store;
    out->observer = observer;
    out->policy = policy;
    out->temperature = temperature;
    out->max_tool_iterations = max_tool_iterations ? max_tool_iterations : 25;
    out->max_history_messages = max_history_messages ? max_history_messages : 50;
    out->auto_save = auto_save;
    out->autonomy_level = autonomy_level;
    out->reflection.enabled = true;
    out->reflection.use_llm = true;
    out->reflection.max_retries = 2;
    out->reflection.min_response_tokens = 0;
    out->custom_instructions = NULL;
    out->custom_instructions_len = 0;
    if (custom_instructions && custom_instructions_len > 0) {
        out->custom_instructions = hu_strndup(alloc, custom_instructions, custom_instructions_len);
        if (!out->custom_instructions)
            return HU_ERR_OUT_OF_MEMORY;
        out->custom_instructions_len = custom_instructions_len;
    }

    out->model_name = hu_strndup(alloc, model_name, model_name_len);
    if (!out->model_name)
        return HU_ERR_OUT_OF_MEMORY;
    out->model_name_len = model_name_len;

    out->default_provider =
        hu_strndup(alloc, default_provider ? default_provider : "openai",
                   default_provider_len ? default_provider_len : strlen("openai"));
    if (!out->default_provider) {
        alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->default_provider_len = default_provider_len ? default_provider_len : strlen("openai");

    out->workspace_dir = hu_strndup(alloc, workspace_dir ? workspace_dir : ".",
                                    workspace_dir_len ? workspace_dir_len : 1);
    if (!out->workspace_dir) {
        alloc->free(alloc->ctx, out->default_provider, out->default_provider_len + 1);
        alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->workspace_dir_len = workspace_dir_len ? workspace_dir_len : 1;

    out->token_limit = 0;
    out->context_pressure_warn = 0.85f;
    out->context_pressure_compact = 0.95f;
    out->context_compact_target = 0.70f;
    out->context_pressure_warning_85_emitted = false;
    out->context_pressure_warning_95_emitted = false;
    if (ctx_cfg) {
        out->token_limit = ctx_cfg->token_limit;
        if (ctx_cfg->pressure_warn > 0.0f)
            out->context_pressure_warn = ctx_cfg->pressure_warn;
        if (ctx_cfg->pressure_compact > 0.0f)
            out->context_pressure_compact = ctx_cfg->pressure_compact;
        if (ctx_cfg->compact_target > 0.0f)
            out->context_compact_target = ctx_cfg->compact_target;
        out->llm_compiler_enabled = ctx_cfg->llm_compiler_enabled;
        out->tool_routing_enabled = ctx_cfg->tool_routing_enabled;
        out->tree_of_thought_enabled = ctx_cfg->tree_of_thought;
        out->constitutional_enabled = ctx_cfg->constitutional_ai;
        if (ctx_cfg->speculative_cache) {
            hu_speculative_cache_t *cache =
                (hu_speculative_cache_t *)alloc->alloc(alloc->ctx, sizeof(hu_speculative_cache_t));
            if (cache && hu_speculative_cache_init(cache, alloc) == HU_OK)
                out->speculative_cache = cache;
        }
    }

    out->tools_count = tools_count;
    if (tools_count > 0) {
        out->tools = (hu_tool_t *)alloc->alloc(alloc->ctx, tools_count * sizeof(hu_tool_t));
        if (!out->tools) {
            alloc->free(alloc->ctx, out->workspace_dir, out->workspace_dir_len + 1);
            alloc->free(alloc->ctx, out->default_provider, out->default_provider_len + 1);
            alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(out->tools, tools, tools_count * sizeof(hu_tool_t));

        out->tool_specs =
            (hu_tool_spec_t *)alloc->alloc(alloc->ctx, tools_count * sizeof(hu_tool_spec_t));
        if (!out->tool_specs) {
            alloc->free(alloc->ctx, out->tools, tools_count * sizeof(hu_tool_t));
            alloc->free(alloc->ctx, out->workspace_dir, out->workspace_dir_len + 1);
            alloc->free(alloc->ctx, out->default_provider, out->default_provider_len + 1);
            alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        out->tool_specs_count = tools_count;
        for (size_t i = 0; i < tools_count; i++) {
            out->tool_specs[i].name = out->tools[i].vtable->name(out->tools[i].ctx);
            out->tool_specs[i].name_len =
                out->tool_specs[i].name ? strlen(out->tool_specs[i].name) : 0;
            out->tool_specs[i].description = out->tools[i].vtable->description(out->tools[i].ctx);
            out->tool_specs[i].description_len =
                out->tool_specs[i].description ? strlen(out->tool_specs[i].description) : 0;
            out->tool_specs[i].parameters_json =
                out->tools[i].vtable->parameters_json(out->tools[i].ctx);
            out->tool_specs[i].parameters_json_len =
                out->tool_specs[i].parameters_json ? strlen(out->tool_specs[i].parameters_json) : 0;
        }
    } else {
        out->tools = NULL;
        out->tool_specs = NULL;
    }

    out->history = NULL;
    out->history_count = 0;
    out->history_cap = 0;
    out->total_tokens = 0;

    /* Build and cache the static portion of the system prompt */
    {
        hu_prompt_config_t cfg = {
            .provider_name = out->provider.vtable->get_name(out->provider.ctx),
            .provider_name_len = 0,
            .model_name = out->model_name,
            .model_name_len = out->model_name_len,
            .workspace_dir = out->workspace_dir,
            .workspace_dir_len = out->workspace_dir_len,
            .tools = out->tools,
            .tools_count = out->tools_count,
            .memory_context = NULL,
            .memory_context_len = 0,
            .autonomy_level = out->autonomy_level,
            .custom_instructions = out->custom_instructions,
            .custom_instructions_len = out->custom_instructions_len,
        };
        hu_error_t perr = hu_prompt_build_static(alloc, &cfg, &out->cached_static_prompt,
                                                 &out->cached_static_prompt_len);
        if (perr != HU_OK) {
            out->cached_static_prompt = NULL;
            out->cached_static_prompt_len = 0;
        }
        out->cached_static_prompt_cap = out->cached_static_prompt_len;
    }

    if (persona && persona_len > 0) {
        out->persona_name = hu_strndup(alloc, persona, persona_len);
        out->persona_name_len = persona_len;
#ifdef HU_HAS_PERSONA
        out->persona = (hu_persona_t *)alloc->alloc(alloc->ctx, sizeof(hu_persona_t));
        if (out->persona) {
            memset(out->persona, 0, sizeof(hu_persona_t));
            hu_error_t perr = hu_persona_load(alloc, persona, persona_len, out->persona);
            if (perr != HU_OK) {
#ifndef HU_IS_TEST
                fprintf(stderr,
                        "[human] warning: persona '%.*s' not found, running without persona\n",
                        (int)persona_len, persona);
#endif
                alloc->free(alloc->ctx, out->persona, sizeof(hu_persona_t));
                out->persona = NULL;
            }
        }
#endif
    }

    out->turn_arena = hu_arena_create(*alloc);

    out->audit_logger = NULL;
    out->undo_stack = hu_undo_stack_create(alloc, 100);
    if (!out->undo_stack) {
        hu_arena_destroy(out->turn_arena);
        out->turn_arena = NULL;
        return HU_ERR_OUT_OF_MEMORY;
    }

    {
        const char *session_id = "default";
        hu_error_t serr = hu_stm_init(&out->stm, *alloc, session_id, 7);
        if (serr != HU_OK)
            return serr;
    }

    {
        hu_error_t rerr = hu_pattern_radar_init(&out->radar, *alloc);
        if (rerr != HU_OK)
            return rerr;
    }

#ifdef HU_HAS_PERSONA
    memset(&out->relationship, 0, sizeof(out->relationship));
    hu_relationship_new_session(&out->relationship);
#endif

    if (memory && memory->vtable) {
        hu_error_t cerr = hu_commitment_store_create(alloc, memory, &out->commitment_store);
        if (cerr != HU_OK)
            out->commitment_store = NULL;
    } else {
        out->commitment_store = NULL;
    }

    /* Superhuman services */
    (void)hu_superhuman_registry_init(&out->superhuman);
    memset(&out->superhuman_commitment_ctx, 0, sizeof(out->superhuman_commitment_ctx));
    memset(&out->superhuman_emotional_ctx, 0, sizeof(out->superhuman_emotional_ctx));
    memset(&out->superhuman_silence_ctx, 0, sizeof(out->superhuman_silence_ctx));

    if (out->commitment_store) {
        out->superhuman_commitment_ctx.store = out->commitment_store;
        out->superhuman_commitment_ctx.session_id = NULL;
        out->superhuman_commitment_ctx.session_id_len = 0;
        hu_superhuman_service_t svc;
        (void)hu_superhuman_commitment_service(&out->superhuman_commitment_ctx, &svc);
        (void)hu_superhuman_register(&out->superhuman, svc);
    }
    {
        hu_superhuman_service_t svc;
        (void)hu_superhuman_predictive_service(&out->radar, &svc);
        (void)hu_superhuman_register(&out->superhuman, svc);
    }
    {
        hu_superhuman_service_t svc;
        (void)hu_superhuman_emotional_service(&out->superhuman_emotional_ctx, &svc);
        (void)hu_superhuman_register(&out->superhuman, svc);
    }
    {
        hu_superhuman_service_t svc;
        (void)hu_superhuman_silence_service(&out->superhuman_silence_ctx, &svc);
        (void)hu_superhuman_register(&out->superhuman, svc);
    }

    return HU_OK;
}

#ifdef HU_HAS_PERSONA
hu_error_t hu_agent_set_persona(hu_agent_t *agent, const char *name, size_t name_len) {
    if (!agent)
        return HU_ERR_INVALID_ARGUMENT;

    /* Free existing persona */
    if (agent->persona) {
        hu_persona_deinit(agent->alloc, agent->persona);
        agent->alloc->free(agent->alloc->ctx, agent->persona, sizeof(hu_persona_t));
        agent->persona = NULL;
    }
    if (agent->persona_name) {
        agent->alloc->free(agent->alloc->ctx, agent->persona_name, agent->persona_name_len + 1);
        agent->persona_name = NULL;
        agent->persona_name_len = 0;
    }
    if (agent->persona_prompt) {
        agent->alloc->free(agent->alloc->ctx, agent->persona_prompt, agent->persona_prompt_len + 1);
        agent->persona_prompt = NULL;
        agent->persona_prompt_len = 0;
    }

    /* If name is NULL or empty, just clear the persona */
    if (!name || name_len == 0)
        return HU_OK;

    /* Load new persona */
    hu_persona_t *new_persona =
        (hu_persona_t *)agent->alloc->alloc(agent->alloc->ctx, sizeof(hu_persona_t));
    if (!new_persona)
        return HU_ERR_OUT_OF_MEMORY;
    memset(new_persona, 0, sizeof(hu_persona_t));

    hu_error_t err = hu_persona_load(agent->alloc, name, name_len, new_persona);
    if (err != HU_OK) {
        agent->alloc->free(agent->alloc->ctx, new_persona, sizeof(hu_persona_t));
        return err;
    }

    agent->persona = new_persona;
    agent->persona_name = hu_strndup(agent->alloc, name, name_len);
    if (!agent->persona_name) {
        hu_persona_deinit(agent->alloc, new_persona);
        agent->alloc->free(agent->alloc->ctx, new_persona, sizeof(hu_persona_t));
        agent->persona = NULL;
        return HU_ERR_OUT_OF_MEMORY;
    }
    agent->persona_name_len = name_len;

    return HU_OK;
}
#endif

void hu_agent_set_mailbox(hu_agent_t *agent, hu_mailbox_t *mailbox) {
    if (!agent)
        return;
    uint64_t id = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
    if (agent->mailbox) {
        hu_error_t err = hu_mailbox_unregister(agent->mailbox, id);
        if (err != HU_OK)
            fprintf(stderr, "warning: mailbox unregister failed: %s\n", hu_error_string(err));
    }
    agent->mailbox = mailbox;
    if (agent->mailbox) {
        hu_error_t err = hu_mailbox_register(agent->mailbox, id);
        if (err != HU_OK)
            fprintf(stderr, "warning: mailbox register failed: %s\n", hu_error_string(err));
    }
}

void hu_agent_set_task_list(hu_agent_t *agent, hu_task_list_t *task_list) {
    if (!agent)
        return;
    agent->task_list = task_list;
}

void hu_agent_set_retrieval_engine(hu_agent_t *agent, hu_retrieval_engine_t *engine) {
    if (!agent)
        return;
    agent->retrieval_engine = engine;
}

void hu_agent_set_awareness(hu_agent_t *agent, struct hu_awareness *awareness) {
    if (!agent)
        return;
    agent->awareness = awareness;
}

const struct hu_awareness *hu_agent_get_awareness(const hu_agent_t *agent) {
    return agent ? agent->awareness : NULL;
}

void hu_agent_set_outcomes(hu_agent_t *agent, struct hu_outcome_tracker *tracker) {
    if (!agent)
        return;
    agent->outcomes = tracker;
}

void hu_agent_deinit(hu_agent_t *agent) {
    if (!agent)
        return;
    if (agent->mailbox) {
        uint64_t id = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
        (void)hu_mailbox_unregister(agent->mailbox, id);
        agent->mailbox = NULL;
    }
    hu_agent_clear_history(agent);
    if (agent->history) {
        agent->alloc->free(agent->alloc->ctx, agent->history,
                           agent->history_cap * sizeof(hu_owned_message_t));
        agent->history = NULL;
        agent->history_cap = 0;
    }
    if (agent->tools) {
        agent->alloc->free(agent->alloc->ctx, agent->tools, agent->tools_count * sizeof(hu_tool_t));
        agent->tools = NULL;
    }
    if (agent->tool_specs) {
        agent->alloc->free(agent->alloc->ctx, agent->tool_specs,
                           agent->tool_specs_count * sizeof(hu_tool_spec_t));
        agent->tool_specs = NULL;
    }
    if (agent->cached_static_prompt) {
        agent->alloc->free(agent->alloc->ctx, agent->cached_static_prompt,
                           agent->cached_static_prompt_cap + 1);
        agent->cached_static_prompt = NULL;
    }
    if (agent->model_name) {
        agent->alloc->free(agent->alloc->ctx, agent->model_name, agent->model_name_len + 1);
        agent->model_name = NULL;
    }
    if (agent->default_provider) {
        agent->alloc->free(agent->alloc->ctx, agent->default_provider,
                           agent->default_provider_len + 1);
        agent->default_provider = NULL;
    }
    if (agent->workspace_dir) {
        agent->alloc->free(agent->alloc->ctx, agent->workspace_dir, agent->workspace_dir_len + 1);
        agent->workspace_dir = NULL;
    }
    if (agent->custom_instructions) {
        agent->alloc->free(agent->alloc->ctx, agent->custom_instructions,
                           agent->custom_instructions_len + 1);
        agent->custom_instructions = NULL;
    }
#ifdef HU_HAS_PERSONA
    if (agent->persona) {
        hu_persona_deinit(agent->alloc, agent->persona);
        agent->alloc->free(agent->alloc->ctx, agent->persona, sizeof(hu_persona_t));
        agent->persona = NULL;
    }
#endif
    if (agent->persona_name) {
        agent->alloc->free(agent->alloc->ctx, agent->persona_name, agent->persona_name_len + 1);
        agent->persona_name = NULL;
    }
    if (agent->persona_prompt) {
        agent->alloc->free(agent->alloc->ctx, agent->persona_prompt, agent->persona_prompt_len + 1);
        agent->persona_prompt = NULL;
    }
    /* Promote important STM entities to persistent memory before cleanup */
    if (agent->memory && agent->memory->vtable) {
        hu_promotion_config_t promo_config = HU_PROMOTION_DEFAULTS;
        (void)hu_promotion_run(agent->alloc, &agent->stm, agent->memory, &promo_config);
    }
    hu_stm_deinit(&agent->stm);
    hu_pattern_radar_deinit(&agent->radar);
    if (agent->commitment_store) {
        hu_commitment_store_destroy(agent->commitment_store);
        agent->commitment_store = NULL;
    }
    if (agent->provider.vtable && agent->provider.vtable->deinit) {
        agent->provider.vtable->deinit(agent->provider.ctx, agent->alloc);
        agent->provider.vtable = NULL;
        agent->provider.ctx = NULL;
    }
    if (agent->turn_arena) {
        hu_arena_destroy(agent->turn_arena);
        agent->turn_arena = NULL;
    }
    if (agent->audit_logger) {
        hu_audit_logger_destroy(agent->audit_logger, agent->alloc);
        agent->audit_logger = NULL;
    }
    if (agent->team) {
        hu_team_destroy(agent->team);
        agent->team = NULL;
    }
    if (agent->undo_stack) {
        hu_undo_stack_destroy(agent->undo_stack);
        agent->undo_stack = NULL;
    }
    if (agent->speculative_cache) {
        hu_speculative_cache_deinit(agent->speculative_cache);
        agent->alloc->free(agent->alloc->ctx, agent->speculative_cache,
                           sizeof(hu_speculative_cache_t));
        agent->speculative_cache = NULL;
    }
}

hu_error_t hu_agent_consolidate_memory(hu_agent_t *agent) {
    if (!agent || !agent->memory || !agent->memory->vtable)
        return HU_ERR_INVALID_ARGUMENT;
    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    config.provider = &agent->provider;
    config.model = agent->model_name;
    config.model_len = agent->model_name_len;
    return hu_memory_consolidate(agent->alloc, agent->memory, &config);
}

hu_error_t hu_agent_internal_ensure_history_cap(hu_agent_t *agent, size_t need) {
    if (agent->history_cap >= need)
        return HU_OK;
    size_t new_cap = agent->history_cap ? agent->history_cap : HU_AGENT_HISTORY_INIT_CAP;
    while (new_cap < need)
        new_cap *= 2;
    hu_owned_message_t *new_arr = (hu_owned_message_t *)agent->alloc->realloc(
        agent->alloc->ctx, agent->history, agent->history_cap * sizeof(hu_owned_message_t),
        new_cap * sizeof(hu_owned_message_t));
    if (!new_arr)
        return HU_ERR_OUT_OF_MEMORY;
    agent->history = new_arr;
    agent->history_cap = new_cap;
    return HU_OK;
}

hu_error_t hu_agent_internal_append_history(hu_agent_t *agent, hu_role_t role, const char *content,
                                            size_t content_len, const char *name, size_t name_len,
                                            const char *tool_call_id, size_t tool_call_id_len) {
    hu_error_t err = hu_agent_internal_ensure_history_cap(agent, agent->history_count + 1);
    if (err != HU_OK)
        return err;
    char *dup = hu_strndup(agent->alloc, content, content_len);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    agent->history[agent->history_count].role = role;
    agent->history[agent->history_count].content = dup;
    agent->history[agent->history_count].content_len = content_len;
    agent->history[agent->history_count].name =
        name_len ? hu_strndup(agent->alloc, name, name_len) : NULL;
    agent->history[agent->history_count].name_len = name_len;
    if (name_len && !agent->history[agent->history_count].name) {
        agent->alloc->free(agent->alloc->ctx, dup, content_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    agent->history[agent->history_count].tool_call_id =
        tool_call_id_len ? hu_strndup(agent->alloc, tool_call_id, tool_call_id_len) : NULL;
    agent->history[agent->history_count].tool_call_id_len = tool_call_id_len;
    if (tool_call_id_len && !agent->history[agent->history_count].tool_call_id) {
        agent->alloc->free(agent->alloc->ctx, dup, content_len + 1);
        if (agent->history[agent->history_count].name)
            agent->alloc->free(agent->alloc->ctx, agent->history[agent->history_count].name,
                               agent->history[agent->history_count].name_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    agent->history[agent->history_count].tool_calls = NULL;
    agent->history[agent->history_count].tool_calls_count = 0;
    agent->history[agent->history_count].content_parts = NULL;
    agent->history[agent->history_count].content_parts_count = 0;
    agent->history_count++;
    return HU_OK;
}

/* Append assistant message with tool_calls (duplicates and owns tool_calls). */
hu_error_t hu_agent_internal_append_history_with_tool_calls(hu_agent_t *agent, const char *content,
                                                            size_t content_len,
                                                            const hu_tool_call_t *tool_calls,
                                                            size_t tool_calls_count) {
    hu_error_t err = hu_agent_internal_ensure_history_cap(agent, agent->history_count + 1);
    if (err != HU_OK)
        return err;
    char *dup = content && content_len > 0 ? hu_strndup(agent->alloc, content, content_len)
                                           : hu_strndup(agent->alloc, "", 0);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    agent->history[agent->history_count].role = HU_ROLE_ASSISTANT;
    agent->history[agent->history_count].content = dup;
    agent->history[agent->history_count].content_len = content_len;
    agent->history[agent->history_count].name = NULL;
    agent->history[agent->history_count].name_len = 0;
    agent->history[agent->history_count].tool_call_id = NULL;
    agent->history[agent->history_count].tool_call_id_len = 0;
    agent->history[agent->history_count].tool_calls = NULL;
    agent->history[agent->history_count].tool_calls_count = 0;
    agent->history[agent->history_count].content_parts = NULL;
    agent->history[agent->history_count].content_parts_count = 0;
    if (tool_calls && tool_calls_count > 0) {
        hu_tool_call_t *owned = (hu_tool_call_t *)agent->alloc->alloc(
            agent->alloc->ctx, tool_calls_count * sizeof(hu_tool_call_t));
        if (!owned) {
            agent->alloc->free(agent->alloc->ctx, dup, content_len + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(owned, 0, tool_calls_count * sizeof(hu_tool_call_t));
        for (size_t i = 0; i < tool_calls_count; i++) {
            const hu_tool_call_t *src = &tool_calls[i];
            if (src->id && src->id_len > 0) {
                owned[i].id = hu_strndup(agent->alloc, src->id, src->id_len);
                if (!owned[i].id) {
                    hu_agent_commands_free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
                    agent->alloc->free(agent->alloc->ctx, dup, content_len ? content_len + 1 : 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                owned[i].id_len = src->id_len;
            }
            if (src->name && src->name_len > 0) {
                owned[i].name = hu_strndup(agent->alloc, src->name, src->name_len);
                if (!owned[i].name) {
                    hu_agent_commands_free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
                    agent->alloc->free(agent->alloc->ctx, dup, content_len ? content_len + 1 : 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                owned[i].name_len = src->name_len;
            }
            if (src->arguments && src->arguments_len > 0) {
                owned[i].arguments = hu_strndup(agent->alloc, src->arguments, src->arguments_len);
                if (!owned[i].arguments) {
                    hu_agent_commands_free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
                    agent->alloc->free(agent->alloc->ctx, dup, content_len ? content_len + 1 : 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                owned[i].arguments_len = src->arguments_len;
            }
        }
        agent->history[agent->history_count].tool_calls = owned;
        agent->history[agent->history_count].tool_calls_count = tool_calls_count;
    }
    agent->history_count++;
    return HU_OK;
}

void hu_agent_clear_history(hu_agent_t *agent) {
    if (!agent || !agent->history)
        return;
    for (size_t i = 0; i < agent->history_count; i++) {
        if (agent->history[i].content) {
            agent->alloc->free(agent->alloc->ctx, agent->history[i].content,
                               agent->history[i].content_len + 1);
        }
        if (agent->history[i].name) {
            agent->alloc->free(agent->alloc->ctx, agent->history[i].name,
                               agent->history[i].name_len + 1);
        }
        if (agent->history[i].tool_call_id) {
            agent->alloc->free(agent->alloc->ctx, agent->history[i].tool_call_id,
                               agent->history[i].tool_call_id_len + 1);
        }
        if (agent->history[i].tool_calls && agent->history[i].tool_calls_count > 0) {
            hu_agent_commands_free_owned_tool_calls(agent->alloc, agent->history[i].tool_calls,
                                                    agent->history[i].tool_calls_count);
            agent->history[i].tool_calls = NULL;
            agent->history[i].tool_calls_count = 0;
        }
        if (agent->history[i].content_parts && agent->history[i].content_parts_count > 0) {
            for (size_t j = 0; j < agent->history[i].content_parts_count; j++) {
                hu_content_part_t *cp = &agent->history[i].content_parts[j];
                if (cp->tag == HU_CONTENT_PART_TEXT && cp->data.text.ptr)
                    agent->alloc->free(agent->alloc->ctx, (void *)cp->data.text.ptr,
                                       cp->data.text.len + 1);
                else if (cp->tag == HU_CONTENT_PART_IMAGE_URL && cp->data.image_url.url)
                    agent->alloc->free(agent->alloc->ctx, (void *)cp->data.image_url.url,
                                       cp->data.image_url.url_len + 1);
                else if (cp->tag == HU_CONTENT_PART_IMAGE_BASE64) {
                    if (cp->data.image_base64.data)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.image_base64.data,
                                           cp->data.image_base64.data_len + 1);
                    if (cp->data.image_base64.media_type)
                        agent->alloc->free(agent->alloc->ctx,
                                           (void *)cp->data.image_base64.media_type,
                                           cp->data.image_base64.media_type_len + 1);
                } else if (cp->tag == HU_CONTENT_PART_AUDIO_BASE64) {
                    if (cp->data.audio_base64.data)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.audio_base64.data,
                                           cp->data.audio_base64.data_len + 1);
                    if (cp->data.audio_base64.media_type)
                        agent->alloc->free(agent->alloc->ctx,
                                           (void *)cp->data.audio_base64.media_type,
                                           cp->data.audio_base64.media_type_len + 1);
                } else if (cp->tag == HU_CONTENT_PART_VIDEO_URL) {
                    if (cp->data.video_url.url)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.video_url.url,
                                           cp->data.video_url.url_len + 1);
                    if (cp->data.video_url.media_type)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.video_url.media_type,
                                           cp->data.video_url.media_type_len + 1);
                }
            }
            agent->alloc->free(agent->alloc->ctx, agent->history[i].content_parts,
                               agent->history[i].content_parts_count * sizeof(hu_content_part_t));
            agent->history[i].content_parts = NULL;
            agent->history[i].content_parts_count = 0;
        }
    }
    agent->history_count = 0;
    if (agent->session_store && agent->session_store->vtable &&
        agent->session_store->vtable->clear_messages) {
        (void)agent->session_store->vtable->clear_messages(agent->session_store->ctx, "", 0);
    }
}

uint32_t hu_agent_estimate_tokens(const char *text, size_t len) {
    if (!text)
        return 0;
    return (uint32_t)((len + 3) / 4);
}

hu_policy_action_t hu_agent_internal_check_policy(hu_agent_t *agent, const char *tool_name,
                                                  const char *arguments) {
    /* First check existing security policy (agent->policy) */
    if (agent->policy && agent->policy->block_high_risk_commands &&
        hu_tool_risk_level(tool_name) == HU_RISK_HIGH)
        return HU_POLICY_DENY;
    /* Then check rule-based policy engine */
    if (!agent->policy_engine)
        return HU_POLICY_ALLOW;
    hu_policy_eval_ctx_t pe_ctx = {
        .tool_name = tool_name, .args_json = arguments ? arguments : "", .session_cost_usd = 0};
    hu_policy_result_t pe_res = hu_policy_engine_evaluate(agent->policy_engine, &pe_ctx);
    return pe_res.action;
}

hu_policy_action_t hu_agent_internal_evaluate_tool_policy(hu_agent_t *agent, const char *tool_name,
                                                          const char *args_json) {
    hu_policy_action_t base = hu_agent_internal_check_policy(agent, tool_name, args_json);
    if (base == HU_POLICY_DENY)
        return HU_POLICY_DENY;
    if (agent->team && agent->agent_id) {
        const hu_team_member_t *member = hu_team_get_member(agent->team, agent->agent_id);
        if (member && !hu_team_role_allows_tool(member->role, tool_name))
            return HU_POLICY_DENY;
    }
    if (agent->policy_engine) {
        hu_policy_eval_ctx_t ctx = {
            .tool_name = tool_name,
            .args_json = args_json ? args_json : "",
            .session_cost_usd = 0.0,
        };
        hu_policy_result_t pr = hu_policy_engine_evaluate(agent->policy_engine, &ctx);
        if (pr.action == HU_POLICY_DENY)
            return HU_POLICY_DENY;
        if (pr.action == HU_POLICY_REQUIRE_APPROVAL)
            return HU_POLICY_REQUIRE_APPROVAL;
    }
    return base;
}

hu_tool_t *hu_agent_internal_find_tool(hu_agent_t *agent, const char *name, size_t name_len) {
    for (size_t i = 0; i < agent->tools_count; i++) {
        const char *n = agent->tools[i].vtable->name(agent->tools[i].ctx);
        if (n && name_len == strlen(n) && memcmp(n, name, name_len) == 0) {
            return &agent->tools[i];
        }
    }
    return NULL;
}

hu_error_t hu_agent_run_single(hu_agent_t *agent, const char *system_prompt,
                               size_t system_prompt_len, const char *user_message,
                               size_t user_message_len, char **response_out,
                               size_t *response_len_out) {
    if (!agent || !response_out)
        return HU_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    hu_chat_message_t msgs[2];
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = system_prompt ? system_prompt : "";
    msgs[0].content_len = system_prompt_len;
    msgs[0].name = NULL;
    msgs[0].name_len = 0;
    msgs[0].tool_call_id = NULL;
    msgs[0].tool_call_id_len = 0;
    msgs[0].content_parts = NULL;
    msgs[0].content_parts_count = 0;

    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = user_message ? user_message : "";
    msgs[1].content_len = user_message_len;
    msgs[1].name = NULL;
    msgs[1].name_len = 0;
    msgs[1].tool_call_id = NULL;
    msgs[1].tool_call_id_len = 0;
    msgs[1].content_parts = NULL;
    msgs[1].content_parts_count = 0;

    req.messages = msgs;
    req.messages_count = 2;
    req.model = agent->model_name;
    req.model_len = agent->model_name_len;
    req.temperature = agent->temperature;
    req.max_tokens = 0;
    req.tools = NULL;
    req.tools_count = 0;
    req.timeout_secs = 0;
    req.reasoning_effort = NULL;
    req.reasoning_effort_len = 0;

    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    hu_error_t err =
        agent->provider.vtable->chat(agent->provider.ctx, agent->alloc, &req, agent->model_name,
                                     agent->model_name_len, agent->temperature, &resp);
    if (err != HU_OK)
        return err;

    if (resp.content && resp.content_len > 0) {
        *response_out = hu_strndup(agent->alloc, resp.content, resp.content_len);
        if (!*response_out) {
            hu_chat_response_free(agent->alloc, &resp);
            return HU_ERR_OUT_OF_MEMORY;
        }
        if (response_len_out)
            *response_len_out = resp.content_len;
    }
    agent->total_tokens += resp.usage.total_tokens;
    hu_agent_internal_record_cost(agent, &resp.usage);
    hu_chat_response_free(agent->alloc, &resp);
    return HU_OK;
}

void hu_agent_internal_maybe_tts(hu_agent_t *agent, const char *text, size_t text_len) {
    if (!agent->tts_enabled || !agent->voice_config || !text || text_len == 0)
        return;
#ifndef HU_IS_TEST
    void *audio = NULL;
    size_t audio_len = 0;
    hu_error_t err =
        hu_voice_tts(agent->alloc, agent->voice_config, text, text_len, &audio, &audio_len);
    if (err == HU_OK && audio && audio_len > 0) {
        hu_voice_play(agent->alloc, audio, audio_len);
        agent->alloc->free(agent->alloc->ctx, audio, audio_len);
    }
#else
    (void)agent;
    (void)text;
    (void)text_len;
#endif
}

void hu_agent_internal_process_mailbox_messages(hu_agent_t *agent) {
    if (!agent->mailbox)
        return;
    uint64_t id = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
    hu_message_t msg;
    while (hu_mailbox_recv(agent->mailbox, id, &msg) == HU_OK) {
        char buf[512];
        size_t payload_len = msg.payload_len < 400 ? msg.payload_len : 400;
        int n = snprintf(buf, sizeof(buf), "[Message from agent %llu]: %.*s",
                         (unsigned long long)msg.from_agent, (int)payload_len,
                         msg.payload ? msg.payload : "");
        if (n > 0)
            (void)hu_agent_internal_append_history(agent, HU_ROLE_USER, buf, (size_t)n, NULL, 0,
                                                   NULL, 0);
        hu_message_free(agent->alloc, &msg);
    }
}
