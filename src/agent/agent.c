#include "seaclaw/agent.h"
#include "seaclaw/agent/awareness.h"
#include "seaclaw/agent/commands.h"
#include "seaclaw/agent/compaction.h"
#include "seaclaw/agent/dispatcher.h"
#include "seaclaw/agent/episodic.h"
#include "seaclaw/agent/input_guard.h"
#include "seaclaw/agent/mailbox.h"
#include "seaclaw/agent/memory_loader.h"
#include "seaclaw/agent/outcomes.h"
#include "seaclaw/agent/planner.h"
#include "seaclaw/agent/preferences.h"
#include "seaclaw/agent/prompt.h"
#include "seaclaw/agent/reflection.h"
#include "seaclaw/agent/task_list.h"
#include "seaclaw/agent/team.h"
#include "seaclaw/agent/tool_context.h"
#include "seaclaw/agent/undo.h"
#include "seaclaw/context.h"
#include "seaclaw/context_tokens.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/observer.h"
#ifdef SC_HAS_PERSONA
#include "seaclaw/persona.h"
#endif
#include "seaclaw/provider.h"
#include "seaclaw/voice.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void sc_agent_internal_generate_trace_id(char *buf) {
    static uint32_t counter = 0;
    uint64_t t = (uint64_t)clock();
    counter++;
    snprintf(buf, 37, "%08x-%04x-%04x-%04x-%08x%04x", (uint32_t)(t & 0xFFFFFFFF),
             (uint16_t)((t >> 32) & 0xFFFF), (uint16_t)(0x4000 | (counter & 0x0FFF)),
             (uint16_t)(0x8000 | ((t >> 16) & 0x3FFF)), (uint32_t)(t * 2654435761u),
             (uint16_t)(counter & 0xFFFF));
}

uint64_t sc_agent_internal_clock_diff_ms(clock_t start, clock_t end) {
    return (uint64_t)((end - start) * 1000 / CLOCKS_PER_SEC);
}

static _Thread_local sc_agent_t *sc__current_agent_for_tools;

void sc_agent_set_current_for_tools(sc_agent_t *agent) {
    sc__current_agent_for_tools = agent;
}
void sc_agent_clear_current_for_tools(void) {
    sc__current_agent_for_tools = NULL;
}
sc_agent_t *sc_agent_get_current_for_tools(void) {
    return sc__current_agent_for_tools;
}

void sc_agent_internal_record_cost(sc_agent_t *agent, const sc_token_usage_t *usage) {
    if (!agent->cost_tracker || !agent->cost_tracker->enabled)
        return;
    sc_cost_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.model = agent->model_name;
    entry.input_tokens = usage->prompt_tokens;
    entry.output_tokens = usage->completion_tokens;
    entry.total_tokens = usage->total_tokens;
    entry.cost_usd = 0.0;
    entry.timestamp_secs = (int64_t)time(NULL);
    (void)sc_cost_record_usage(agent->cost_tracker, &entry, agent->active_job_id);
}

#define SC_AGENT_HISTORY_INIT_CAP 16
#define SC_AGENT_MAX_SLASH_LEN    256

sc_error_t sc_agent_from_config(
    sc_agent_t *out, sc_allocator_t *alloc, sc_provider_t provider, const sc_tool_t *tools,
    size_t tools_count, sc_memory_t *memory, sc_session_store_t *session_store,
    sc_observer_t *observer, sc_security_policy_t *policy, const char *model_name,
    size_t model_name_len, const char *default_provider, size_t default_provider_len,
    double temperature, const char *workspace_dir, size_t workspace_dir_len,
    uint32_t max_tool_iterations, uint32_t max_history_messages, bool auto_save,
    uint8_t autonomy_level, const char *custom_instructions, size_t custom_instructions_len,
    const char *persona, size_t persona_len, const sc_agent_context_config_t *ctx_cfg) {
    if (!out || !alloc || !provider.vtable || !model_name) {
        return SC_ERR_INVALID_ARGUMENT;
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
        out->custom_instructions = sc_strndup(alloc, custom_instructions, custom_instructions_len);
        if (!out->custom_instructions)
            return SC_ERR_OUT_OF_MEMORY;
        out->custom_instructions_len = custom_instructions_len;
    }

    out->model_name = sc_strndup(alloc, model_name, model_name_len);
    if (!out->model_name)
        return SC_ERR_OUT_OF_MEMORY;
    out->model_name_len = model_name_len;

    out->default_provider =
        sc_strndup(alloc, default_provider ? default_provider : "openai",
                   default_provider_len ? default_provider_len : strlen("openai"));
    if (!out->default_provider) {
        alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    out->default_provider_len = default_provider_len ? default_provider_len : strlen("openai");

    out->workspace_dir = sc_strndup(alloc, workspace_dir ? workspace_dir : ".",
                                    workspace_dir_len ? workspace_dir_len : 1);
    if (!out->workspace_dir) {
        alloc->free(alloc->ctx, out->default_provider, out->default_provider_len + 1);
        alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
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
    }

    out->tools_count = tools_count;
    if (tools_count > 0) {
        out->tools = (sc_tool_t *)alloc->alloc(alloc->ctx, tools_count * sizeof(sc_tool_t));
        if (!out->tools) {
            alloc->free(alloc->ctx, out->workspace_dir, out->workspace_dir_len + 1);
            alloc->free(alloc->ctx, out->default_provider, out->default_provider_len + 1);
            alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(out->tools, tools, tools_count * sizeof(sc_tool_t));

        out->tool_specs =
            (sc_tool_spec_t *)alloc->alloc(alloc->ctx, tools_count * sizeof(sc_tool_spec_t));
        if (!out->tool_specs) {
            alloc->free(alloc->ctx, out->tools, tools_count * sizeof(sc_tool_t));
            alloc->free(alloc->ctx, out->workspace_dir, out->workspace_dir_len + 1);
            alloc->free(alloc->ctx, out->default_provider, out->default_provider_len + 1);
            alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
            return SC_ERR_OUT_OF_MEMORY;
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
        sc_prompt_config_t cfg = {
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
        sc_error_t perr = sc_prompt_build_static(alloc, &cfg, &out->cached_static_prompt,
                                                 &out->cached_static_prompt_len);
        if (perr != SC_OK) {
            out->cached_static_prompt = NULL;
            out->cached_static_prompt_len = 0;
        }
        out->cached_static_prompt_cap = out->cached_static_prompt_len;
    }

    if (persona && persona_len > 0) {
        out->persona_name = sc_strndup(alloc, persona, persona_len);
        out->persona_name_len = persona_len;
#ifdef SC_HAS_PERSONA
        out->persona = (sc_persona_t *)alloc->alloc(alloc->ctx, sizeof(sc_persona_t));
        if (out->persona) {
            memset(out->persona, 0, sizeof(sc_persona_t));
            sc_error_t perr = sc_persona_load(alloc, persona, persona_len, out->persona);
            if (perr != SC_OK) {
#ifndef SC_IS_TEST
                fprintf(stderr,
                        "[seaclaw] warning: persona '%.*s' not found, running without persona\n",
                        (int)persona_len, persona);
#endif
                alloc->free(alloc->ctx, out->persona, sizeof(sc_persona_t));
                out->persona = NULL;
            }
        }
#endif
    }

    out->turn_arena = sc_arena_create(*alloc);

    out->audit_logger = NULL;
    out->undo_stack = sc_undo_stack_create(alloc, 100);
    if (!out->undo_stack) {
        sc_arena_destroy(out->turn_arena);
        out->turn_arena = NULL;
        return SC_ERR_OUT_OF_MEMORY;
    }

    return SC_OK;
}

#ifdef SC_HAS_PERSONA
sc_error_t sc_agent_set_persona(sc_agent_t *agent, const char *name, size_t name_len) {
    if (!agent)
        return SC_ERR_INVALID_ARGUMENT;

    /* Free existing persona */
    if (agent->persona) {
        sc_persona_deinit(agent->alloc, agent->persona);
        agent->alloc->free(agent->alloc->ctx, agent->persona, sizeof(sc_persona_t));
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
        return SC_OK;

    /* Load new persona */
    sc_persona_t *new_persona =
        (sc_persona_t *)agent->alloc->alloc(agent->alloc->ctx, sizeof(sc_persona_t));
    if (!new_persona)
        return SC_ERR_OUT_OF_MEMORY;
    memset(new_persona, 0, sizeof(sc_persona_t));

    sc_error_t err = sc_persona_load(agent->alloc, name, name_len, new_persona);
    if (err != SC_OK) {
        agent->alloc->free(agent->alloc->ctx, new_persona, sizeof(sc_persona_t));
        return err;
    }

    agent->persona = new_persona;
    agent->persona_name = sc_strndup(agent->alloc, name, name_len);
    if (!agent->persona_name) {
        sc_persona_deinit(agent->alloc, new_persona);
        agent->alloc->free(agent->alloc->ctx, new_persona, sizeof(sc_persona_t));
        agent->persona = NULL;
        return SC_ERR_OUT_OF_MEMORY;
    }
    agent->persona_name_len = name_len;

    return SC_OK;
}
#endif

void sc_agent_set_mailbox(sc_agent_t *agent, sc_mailbox_t *mailbox) {
    if (!agent)
        return;
    uint64_t id = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
    if (agent->mailbox) {
        sc_error_t err = sc_mailbox_unregister(agent->mailbox, id);
        if (err != SC_OK)
            fprintf(stderr, "warning: mailbox unregister failed: %s\n", sc_error_string(err));
    }
    agent->mailbox = mailbox;
    if (agent->mailbox) {
        sc_error_t err = sc_mailbox_register(agent->mailbox, id);
        if (err != SC_OK)
            fprintf(stderr, "warning: mailbox register failed: %s\n", sc_error_string(err));
    }
}

void sc_agent_set_task_list(sc_agent_t *agent, sc_task_list_t *task_list) {
    if (!agent)
        return;
    agent->task_list = task_list;
}

void sc_agent_set_retrieval_engine(sc_agent_t *agent, sc_retrieval_engine_t *engine) {
    if (!agent)
        return;
    agent->retrieval_engine = engine;
}

void sc_agent_set_awareness(sc_agent_t *agent, struct sc_awareness *awareness) {
    if (!agent)
        return;
    agent->awareness = awareness;
}

const struct sc_awareness *sc_agent_get_awareness(const sc_agent_t *agent) {
    return agent ? agent->awareness : NULL;
}

void sc_agent_set_outcomes(sc_agent_t *agent, struct sc_outcome_tracker *tracker) {
    if (!agent)
        return;
    agent->outcomes = tracker;
}

void sc_agent_deinit(sc_agent_t *agent) {
    if (!agent)
        return;
    if (agent->mailbox) {
        uint64_t id = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
        (void)sc_mailbox_unregister(agent->mailbox, id);
        agent->mailbox = NULL;
    }
    sc_agent_clear_history(agent);
    if (agent->history) {
        agent->alloc->free(agent->alloc->ctx, agent->history,
                           agent->history_cap * sizeof(sc_owned_message_t));
        agent->history = NULL;
        agent->history_cap = 0;
    }
    if (agent->tools) {
        agent->alloc->free(agent->alloc->ctx, agent->tools, agent->tools_count * sizeof(sc_tool_t));
        agent->tools = NULL;
    }
    if (agent->tool_specs) {
        agent->alloc->free(agent->alloc->ctx, agent->tool_specs,
                           agent->tool_specs_count * sizeof(sc_tool_spec_t));
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
#ifdef SC_HAS_PERSONA
    if (agent->persona) {
        sc_persona_deinit(agent->alloc, agent->persona);
        agent->alloc->free(agent->alloc->ctx, agent->persona, sizeof(sc_persona_t));
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
    if (agent->provider.vtable && agent->provider.vtable->deinit) {
        agent->provider.vtable->deinit(agent->provider.ctx, agent->alloc);
    }
    if (agent->turn_arena) {
        sc_arena_destroy(agent->turn_arena);
        agent->turn_arena = NULL;
    }
    if (agent->audit_logger) {
        sc_audit_logger_destroy(agent->audit_logger, agent->alloc);
        agent->audit_logger = NULL;
    }
    if (agent->team) {
        sc_team_destroy(agent->team);
        agent->team = NULL;
    }
    if (agent->undo_stack) {
        sc_undo_stack_destroy(agent->undo_stack);
        agent->undo_stack = NULL;
    }
}

sc_error_t sc_agent_internal_ensure_history_cap(sc_agent_t *agent, size_t need) {
    if (agent->history_cap >= need)
        return SC_OK;
    size_t new_cap = agent->history_cap ? agent->history_cap : SC_AGENT_HISTORY_INIT_CAP;
    while (new_cap < need)
        new_cap *= 2;
    sc_owned_message_t *new_arr = (sc_owned_message_t *)agent->alloc->realloc(
        agent->alloc->ctx, agent->history, agent->history_cap * sizeof(sc_owned_message_t),
        new_cap * sizeof(sc_owned_message_t));
    if (!new_arr)
        return SC_ERR_OUT_OF_MEMORY;
    agent->history = new_arr;
    agent->history_cap = new_cap;
    return SC_OK;
}

sc_error_t sc_agent_internal_append_history(sc_agent_t *agent, sc_role_t role, const char *content,
                                            size_t content_len, const char *name, size_t name_len,
                                            const char *tool_call_id, size_t tool_call_id_len) {
    sc_error_t err = sc_agent_internal_ensure_history_cap(agent, agent->history_count + 1);
    if (err != SC_OK)
        return err;
    char *dup = sc_strndup(agent->alloc, content, content_len);
    if (!dup)
        return SC_ERR_OUT_OF_MEMORY;
    agent->history[agent->history_count].role = role;
    agent->history[agent->history_count].content = dup;
    agent->history[agent->history_count].content_len = content_len;
    agent->history[agent->history_count].name =
        name_len ? sc_strndup(agent->alloc, name, name_len) : NULL;
    agent->history[agent->history_count].name_len = name_len;
    if (name_len && !agent->history[agent->history_count].name) {
        agent->alloc->free(agent->alloc->ctx, dup, content_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    agent->history[agent->history_count].tool_call_id =
        tool_call_id_len ? sc_strndup(agent->alloc, tool_call_id, tool_call_id_len) : NULL;
    agent->history[agent->history_count].tool_call_id_len = tool_call_id_len;
    if (tool_call_id_len && !agent->history[agent->history_count].tool_call_id) {
        agent->alloc->free(agent->alloc->ctx, dup, content_len + 1);
        if (agent->history[agent->history_count].name)
            agent->alloc->free(agent->alloc->ctx, agent->history[agent->history_count].name,
                               agent->history[agent->history_count].name_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    agent->history[agent->history_count].tool_calls = NULL;
    agent->history[agent->history_count].tool_calls_count = 0;
    agent->history[agent->history_count].content_parts = NULL;
    agent->history[agent->history_count].content_parts_count = 0;
    agent->history_count++;
    return SC_OK;
}

/* Append assistant message with tool_calls (duplicates and owns tool_calls). */
sc_error_t sc_agent_internal_append_history_with_tool_calls(sc_agent_t *agent, const char *content,
                                                            size_t content_len,
                                                            const sc_tool_call_t *tool_calls,
                                                            size_t tool_calls_count) {
    sc_error_t err = sc_agent_internal_ensure_history_cap(agent, agent->history_count + 1);
    if (err != SC_OK)
        return err;
    char *dup = content && content_len > 0 ? sc_strndup(agent->alloc, content, content_len)
                                           : sc_strndup(agent->alloc, "", 0);
    if (!dup)
        return SC_ERR_OUT_OF_MEMORY;
    agent->history[agent->history_count].role = SC_ROLE_ASSISTANT;
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
        sc_tool_call_t *owned = (sc_tool_call_t *)agent->alloc->alloc(
            agent->alloc->ctx, tool_calls_count * sizeof(sc_tool_call_t));
        if (!owned) {
            agent->alloc->free(agent->alloc->ctx, dup, content_len + 1);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memset(owned, 0, tool_calls_count * sizeof(sc_tool_call_t));
        for (size_t i = 0; i < tool_calls_count; i++) {
            const sc_tool_call_t *src = &tool_calls[i];
            if (src->id && src->id_len > 0) {
                owned[i].id = sc_strndup(agent->alloc, src->id, src->id_len);
                if (!owned[i].id) {
                    sc_agent_commands_free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
                    agent->alloc->free(agent->alloc->ctx, dup, content_len ? content_len + 1 : 1);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                owned[i].id_len = src->id_len;
            }
            if (src->name && src->name_len > 0) {
                owned[i].name = sc_strndup(agent->alloc, src->name, src->name_len);
                if (!owned[i].name) {
                    sc_agent_commands_free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
                    agent->alloc->free(agent->alloc->ctx, dup, content_len ? content_len + 1 : 1);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                owned[i].name_len = src->name_len;
            }
            if (src->arguments && src->arguments_len > 0) {
                owned[i].arguments = sc_strndup(agent->alloc, src->arguments, src->arguments_len);
                if (!owned[i].arguments) {
                    sc_agent_commands_free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
                    agent->alloc->free(agent->alloc->ctx, dup, content_len ? content_len + 1 : 1);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                owned[i].arguments_len = src->arguments_len;
            }
        }
        agent->history[agent->history_count].tool_calls = owned;
        agent->history[agent->history_count].tool_calls_count = tool_calls_count;
    }
    agent->history_count++;
    return SC_OK;
}

void sc_agent_clear_history(sc_agent_t *agent) {
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
            sc_agent_commands_free_owned_tool_calls(agent->alloc, agent->history[i].tool_calls,
                                                    agent->history[i].tool_calls_count);
            agent->history[i].tool_calls = NULL;
            agent->history[i].tool_calls_count = 0;
        }
        if (agent->history[i].content_parts && agent->history[i].content_parts_count > 0) {
            for (size_t j = 0; j < agent->history[i].content_parts_count; j++) {
                sc_content_part_t *cp = &agent->history[i].content_parts[j];
                if (cp->tag == SC_CONTENT_PART_TEXT && cp->data.text.ptr)
                    agent->alloc->free(agent->alloc->ctx, (void *)cp->data.text.ptr,
                                       cp->data.text.len + 1);
                else if (cp->tag == SC_CONTENT_PART_IMAGE_URL && cp->data.image_url.url)
                    agent->alloc->free(agent->alloc->ctx, (void *)cp->data.image_url.url,
                                       cp->data.image_url.url_len + 1);
                else if (cp->tag == SC_CONTENT_PART_IMAGE_BASE64) {
                    if (cp->data.image_base64.data)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.image_base64.data,
                                           cp->data.image_base64.data_len + 1);
                    if (cp->data.image_base64.media_type)
                        agent->alloc->free(agent->alloc->ctx,
                                           (void *)cp->data.image_base64.media_type,
                                           cp->data.image_base64.media_type_len + 1);
                } else if (cp->tag == SC_CONTENT_PART_AUDIO_BASE64) {
                    if (cp->data.audio_base64.data)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.audio_base64.data,
                                           cp->data.audio_base64.data_len + 1);
                    if (cp->data.audio_base64.media_type)
                        agent->alloc->free(agent->alloc->ctx,
                                           (void *)cp->data.audio_base64.media_type,
                                           cp->data.audio_base64.media_type_len + 1);
                } else if (cp->tag == SC_CONTENT_PART_VIDEO_URL) {
                    if (cp->data.video_url.url)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.video_url.url,
                                           cp->data.video_url.url_len + 1);
                    if (cp->data.video_url.media_type)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.video_url.media_type,
                                           cp->data.video_url.media_type_len + 1);
                }
            }
            agent->alloc->free(agent->alloc->ctx, agent->history[i].content_parts,
                               agent->history[i].content_parts_count * sizeof(sc_content_part_t));
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

uint32_t sc_agent_estimate_tokens(const char *text, size_t len) {
    if (!text)
        return 0;
    return (uint32_t)((len + 3) / 4);
}

sc_policy_action_t sc_agent_internal_check_policy(sc_agent_t *agent, const char *tool_name,
                                                  const char *arguments) {
    /* First check existing security policy (agent->policy) */
    if (agent->policy && agent->policy->block_high_risk_commands &&
        sc_tool_risk_level(tool_name) == SC_RISK_HIGH)
        return SC_POLICY_DENY;
    /* Then check rule-based policy engine */
    if (!agent->policy_engine)
        return SC_POLICY_ALLOW;
    sc_policy_eval_ctx_t pe_ctx = {
        .tool_name = tool_name, .args_json = arguments ? arguments : "", .session_cost_usd = 0};
    sc_policy_result_t pe_res = sc_policy_engine_evaluate(agent->policy_engine, &pe_ctx);
    return pe_res.action;
}

sc_policy_action_t sc_agent_internal_evaluate_tool_policy(sc_agent_t *agent, const char *tool_name,
                                                          const char *args_json) {
    sc_policy_action_t base = sc_agent_internal_check_policy(agent, tool_name, args_json);
    if (base == SC_POLICY_DENY)
        return SC_POLICY_DENY;
    if (agent->team && agent->agent_id) {
        const sc_team_member_t *member = sc_team_get_member(agent->team, agent->agent_id);
        if (member && !sc_team_role_allows_tool(member->role, tool_name))
            return SC_POLICY_DENY;
    }
    if (agent->policy_engine) {
        sc_policy_eval_ctx_t ctx = {
            .tool_name = tool_name,
            .args_json = args_json ? args_json : "",
            .session_cost_usd = 0.0,
        };
        sc_policy_result_t pr = sc_policy_engine_evaluate(agent->policy_engine, &ctx);
        if (pr.action == SC_POLICY_DENY)
            return SC_POLICY_DENY;
        if (pr.action == SC_POLICY_REQUIRE_APPROVAL)
            return SC_POLICY_REQUIRE_APPROVAL;
    }
    return base;
}

sc_tool_t *sc_agent_internal_find_tool(sc_agent_t *agent, const char *name, size_t name_len) {
    for (size_t i = 0; i < agent->tools_count; i++) {
        const char *n = agent->tools[i].vtable->name(agent->tools[i].ctx);
        if (n && name_len == strlen(n) && memcmp(n, name, name_len) == 0) {
            return &agent->tools[i];
        }
    }
    return NULL;
}

sc_error_t sc_agent_run_single(sc_agent_t *agent, const char *system_prompt,
                               size_t system_prompt_len, const char *user_message,
                               size_t user_message_len, char **response_out,
                               size_t *response_len_out) {
    if (!agent || !response_out)
        return SC_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    sc_chat_request_t req;
    memset(&req, 0, sizeof(req));
    sc_chat_message_t msgs[2];
    msgs[0].role = SC_ROLE_SYSTEM;
    msgs[0].content = system_prompt ? system_prompt : "";
    msgs[0].content_len = system_prompt_len;
    msgs[0].name = NULL;
    msgs[0].name_len = 0;
    msgs[0].tool_call_id = NULL;
    msgs[0].tool_call_id_len = 0;
    msgs[0].content_parts = NULL;
    msgs[0].content_parts_count = 0;

    msgs[1].role = SC_ROLE_USER;
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

    sc_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    sc_error_t err =
        agent->provider.vtable->chat(agent->provider.ctx, agent->alloc, &req, agent->model_name,
                                     agent->model_name_len, agent->temperature, &resp);
    if (err != SC_OK)
        return err;

    if (resp.content && resp.content_len > 0) {
        *response_out = sc_strndup(agent->alloc, resp.content, resp.content_len);
        if (!*response_out) {
            sc_chat_response_free(agent->alloc, &resp);
            return SC_ERR_OUT_OF_MEMORY;
        }
        if (response_len_out)
            *response_len_out = resp.content_len;
    }
    agent->total_tokens += resp.usage.total_tokens;
    sc_agent_internal_record_cost(agent, &resp.usage);
    sc_chat_response_free(agent->alloc, &resp);
    return SC_OK;
}

void sc_agent_internal_maybe_tts(sc_agent_t *agent, const char *text, size_t text_len) {
    if (!agent->tts_enabled || !agent->voice_config || !text || text_len == 0)
        return;
#ifndef SC_IS_TEST
    void *audio = NULL;
    size_t audio_len = 0;
    sc_error_t err =
        sc_voice_tts(agent->alloc, agent->voice_config, text, text_len, &audio, &audio_len);
    if (err == SC_OK && audio && audio_len > 0) {
        sc_voice_play(agent->alloc, audio, audio_len);
        agent->alloc->free(agent->alloc->ctx, audio, audio_len);
    }
#else
    (void)agent;
    (void)text;
    (void)text_len;
#endif
}

void sc_agent_internal_process_mailbox_messages(sc_agent_t *agent) {
    if (!agent->mailbox)
        return;
    uint64_t id = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
    sc_message_t msg;
    while (sc_mailbox_recv(agent->mailbox, id, &msg) == SC_OK) {
        char buf[512];
        size_t payload_len = msg.payload_len < 400 ? msg.payload_len : 400;
        int n = snprintf(buf, sizeof(buf), "[Message from agent %llu]: %.*s",
                         (unsigned long long)msg.from_agent, (int)payload_len,
                         msg.payload ? msg.payload : "");
        if (n > 0)
            (void)sc_agent_internal_append_history(agent, SC_ROLE_USER, buf, (size_t)n, NULL, 0,
                                                   NULL, 0);
        sc_message_free(agent->alloc, &msg);
    }
}
