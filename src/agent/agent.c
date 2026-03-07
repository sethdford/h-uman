#include "seaclaw/agent.h"
#include "seaclaw/agent/awareness.h"
#include "seaclaw/agent/compaction.h"
#include "seaclaw/agent/dispatcher.h"
#include "seaclaw/agent/episodic.h"
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

#define SC_OBS_SAFE_RECORD_EVENT(agent, ev)                     \
    do {                                                        \
        if ((agent)->observer)                                  \
            sc_observer_record_event(*(agent)->observer, (ev)); \
    } while (0)

static uint64_t clock_diff_ms(clock_t start, clock_t end) {
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

static void agent_record_cost(sc_agent_t *agent, const sc_token_usage_t *usage) {
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
    sc_cost_record_usage(agent->cost_tracker, &entry);
}

#define SC_AGENT_HISTORY_INIT_CAP 16
#define SC_AGENT_MAX_SLASH_LEN    256

static bool is_slash_command(const char *msg, size_t len) {
    if (!msg || len == 0)
        return false;
    size_t i = 0;
    while (i < len && (msg[i] == ' ' || msg[i] == '\t'))
        i++;
    if (i >= len)
        return false;
    return msg[i] == '/';
}

static void parse_slash(const char *msg, size_t len, char *cmd_buf, size_t cmd_cap, size_t *cmd_len,
                        char *arg_buf, size_t arg_cap, size_t *arg_len) {
    *cmd_len = 0;
    *arg_len = 0;
    if (cmd_cap)
        cmd_buf[0] = '\0';
    if (arg_cap)
        arg_buf[0] = '\0';
    size_t i = 0;
    while (i < len && (msg[i] == ' ' || msg[i] == '\t'))
        i++;
    if (i >= len || msg[i] != '/')
        return;
    i++;
    size_t cmd_start = i;
    while (i < len && msg[i] != ' ' && msg[i] != '\t' && msg[i] != ':')
        i++;
    size_t cmd_end = i;
    if (cmd_end > cmd_start && cmd_end - cmd_start < cmd_cap) {
        memcpy(cmd_buf, msg + cmd_start, cmd_end - cmd_start);
        cmd_buf[cmd_end - cmd_start] = '\0';
        *cmd_len = cmd_end - cmd_start;
    }
    while (i < len && (msg[i] == ' ' || msg[i] == '\t' || msg[i] == ':'))
        i++;
    size_t arg_start = i;
    while (i < len && msg[i] != '\n' && msg[i] != '\r')
        i++;
    size_t arg_end = i;
    while (arg_end > arg_start && (msg[arg_end - 1] == ' ' || msg[arg_end - 1] == '\t'))
        arg_end--;
    if (arg_end > arg_start && arg_end - arg_start < arg_cap) {
        memcpy(arg_buf, msg + arg_start, arg_end - arg_start);
        arg_buf[arg_end - arg_start] = '\0';
        *arg_len = arg_end - arg_start;
    }
}

static int sc_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = (char)tolower((unsigned char)(a[i]));
        char cb = (char)tolower((unsigned char)(b[i]));
        if (ca != cb)
            return (int)(unsigned char)ca - (unsigned char)cb;
        if (ca == '\0')
            return 0;
    }
    return 0;
}

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
    out->reflection.max_retries = 1;
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
    if (agent->mailbox)
        sc_mailbox_unregister(agent->mailbox, id);
    agent->mailbox = mailbox;
    if (agent->mailbox)
        sc_mailbox_register(agent->mailbox, id);
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
        sc_mailbox_unregister(agent->mailbox, id);
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

static sc_error_t ensure_history_cap(sc_agent_t *agent, size_t need) {
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

static void free_owned_tool_calls(sc_allocator_t *alloc, sc_tool_call_t *tcs, size_t count);

static sc_error_t append_history(sc_agent_t *agent, sc_role_t role, const char *content,
                                 size_t content_len, const char *name, size_t name_len,
                                 const char *tool_call_id, size_t tool_call_id_len) {
    sc_error_t err = ensure_history_cap(agent, agent->history_count + 1);
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
    agent->history_count++;
    return SC_OK;
}

/* Append assistant message with tool_calls (duplicates and owns tool_calls). */
static sc_error_t append_history_with_tool_calls(sc_agent_t *agent, const char *content,
                                                 size_t content_len,
                                                 const sc_tool_call_t *tool_calls,
                                                 size_t tool_calls_count) {
    sc_error_t err = ensure_history_cap(agent, agent->history_count + 1);
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
                    free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
                    agent->alloc->free(agent->alloc->ctx, dup, content_len ? content_len + 1 : 1);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                owned[i].id_len = src->id_len;
            }
            if (src->name && src->name_len > 0) {
                owned[i].name = sc_strndup(agent->alloc, src->name, src->name_len);
                if (!owned[i].name) {
                    free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
                    agent->alloc->free(agent->alloc->ctx, dup, content_len ? content_len + 1 : 1);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                owned[i].name_len = src->name_len;
            }
            if (src->arguments && src->arguments_len > 0) {
                owned[i].arguments = sc_strndup(agent->alloc, src->arguments, src->arguments_len);
                if (!owned[i].arguments) {
                    free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
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

static void free_owned_tool_calls(sc_allocator_t *alloc, sc_tool_call_t *tcs, size_t count) {
    if (!tcs || count == 0)
        return;
    for (size_t i = 0; i < count; i++) {
        if (tcs[i].id)
            alloc->free(alloc->ctx, (void *)tcs[i].id, tcs[i].id_len + 1);
        if (tcs[i].name)
            alloc->free(alloc->ctx, (void *)tcs[i].name, tcs[i].name_len + 1);
        if (tcs[i].arguments)
            alloc->free(alloc->ctx, (void *)tcs[i].arguments, tcs[i].arguments_len + 1);
    }
    alloc->free(alloc->ctx, tcs, count * sizeof(sc_tool_call_t));
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
            free_owned_tool_calls(agent->alloc, agent->history[i].tool_calls,
                                  agent->history[i].tool_calls_count);
            agent->history[i].tool_calls = NULL;
            agent->history[i].tool_calls_count = 0;
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

char *sc_agent_handle_slash_command(sc_agent_t *agent, const char *message, size_t message_len) {
    if (!agent || !message || !is_slash_command(message, message_len)) {
        return NULL;
    }
    char cmd_buf[64], arg_buf[192];
    size_t cmd_len, arg_len;
    parse_slash(message, message_len, cmd_buf, sizeof(cmd_buf), &cmd_len, arg_buf, sizeof(arg_buf),
                &arg_len);

    if (cmd_len == 0)
        return NULL;

    if (sc_strncasecmp(cmd_buf, "help", 4) == 0 || sc_strncasecmp(cmd_buf, "commands", 8) == 0) {
        const char *help = "Commands:\n"
                           "  /help, /commands   Show this help\n"
                           "  /quit, /exit      End session\n"
                           "  /clear            Clear conversation history\n"
                           "  /sessions         List active sessions\n"
                           "  /kill             Clear history and reset session\n"
                           "  /retry            Remove last response; resend to retry\n"
                           "  /model [name]     Show or switch model\n"
                           "  /provider         Show current provider\n"
                           "  /tools            List available tools\n"
                           "  /plan <json>      Execute a structured plan\n"
                           "  /goal <text>      Generate and execute plan from a goal\n"
                           "  /cost             Show token usage\n"
                           "  /status           Show agent status\n"
                           "  /spawn <task>     Spawn a sub-agent\n"
                           "  /agents           List running agents\n"
                           "  /cancel <id>      Cancel a sub-agent\n"
                           "  /send <id> <msg>  Send message to another agent\n"
                           "  /tasks            Show task list summary\n"
                           "  /task add <subj>   Create task\n"
                           "  /task claim <id>   Claim task\n"
                           "  /task done <id>    Mark task complete\n"
                           "  /undo             Undo last reversible action\n";
        return sc_strndup(agent->alloc, help, strlen(help));
    }

    if (sc_strncasecmp(cmd_buf, "quit", 4) == 0 || sc_strncasecmp(cmd_buf, "exit", 4) == 0) {
        return sc_strndup(agent->alloc, "Goodbye.", 8);
    }

    if (sc_strncasecmp(cmd_buf, "clear", 5) == 0 || sc_strncasecmp(cmd_buf, "new", 3) == 0 ||
        sc_strncasecmp(cmd_buf, "reset", 5) == 0) {
        sc_agent_clear_history(agent);
        return sc_strndup(agent->alloc, "History cleared.", 16);
    }

    if (sc_strncasecmp(cmd_buf, "sessions", 8) == 0) {
        return sc_strndup(agent->alloc, "Active sessions:\n- default (current)\n", 42);
    }

    if (sc_strncasecmp(cmd_buf, "kill", 4) == 0) {
        sc_agent_clear_history(agent);
        return sc_strndup(agent->alloc, "Session killed. History cleared.", 33);
    }

    if (sc_strncasecmp(cmd_buf, "retry", 5) == 0) {
        if (agent->history_count > 0) {
            sc_owned_message_t *last = &agent->history[agent->history_count - 1];
            if (last->role != SC_ROLE_ASSISTANT) {
                return sc_strndup(agent->alloc,
                                  "Nothing to retry. Last message is not from assistant.", 53);
            }
            if (last->content) {
                agent->alloc->free(agent->alloc->ctx, last->content, last->content_len + 1);
                last->content = NULL;
            }
            if (last->name) {
                agent->alloc->free(agent->alloc->ctx, last->name, last->name_len + 1);
                last->name = NULL;
            }
            if (last->tool_call_id) {
                agent->alloc->free(agent->alloc->ctx, last->tool_call_id,
                                   last->tool_call_id_len + 1);
                last->tool_call_id = NULL;
            }
            if (last->tool_calls && last->tool_calls_count > 0) {
                free_owned_tool_calls(agent->alloc, last->tool_calls, last->tool_calls_count);
                last->tool_calls = NULL;
                last->tool_calls_count = 0;
            }
            agent->history_count--;
        }
        return sc_strndup(agent->alloc, "Last response removed. Send your message again to retry.",
                          57);
    }

    if (sc_strncasecmp(cmd_buf, "model", 5) == 0) {
        if (arg_len > 0) {
            char *old = agent->model_name;
            size_t old_len = agent->model_name_len;
            agent->model_name = sc_strndup(agent->alloc, arg_buf, arg_len);
            if (!agent->model_name)
                return NULL;
            agent->model_name_len = arg_len;
            if (old)
                agent->alloc->free(agent->alloc->ctx, old, old_len + 1);

            /* Invalidate and rebuild static prompt cache for new model */
            if (agent->cached_static_prompt) {
                agent->alloc->free(agent->alloc->ctx, agent->cached_static_prompt,
                                   agent->cached_static_prompt_len + 1);
                agent->cached_static_prompt = NULL;
                agent->cached_static_prompt_len = 0;
                agent->cached_static_prompt_cap = 0;
            }
            sc_prompt_config_t pcfg = {
                .provider_name = agent->provider.vtable->get_name(agent->provider.ctx),
                .provider_name_len = 0,
                .model_name = agent->model_name,
                .model_name_len = agent->model_name_len,
                .workspace_dir = agent->workspace_dir,
                .workspace_dir_len = agent->workspace_dir_len,
                .tools = agent->tools,
                .tools_count = agent->tools_count,
                .memory_context = NULL,
                .memory_context_len = 0,
                .autonomy_level = agent->autonomy_level,
                .custom_instructions = agent->custom_instructions,
                .custom_instructions_len = agent->custom_instructions_len,
            };
            (void)sc_prompt_build_static(agent->alloc, &pcfg, &agent->cached_static_prompt,
                                         &agent->cached_static_prompt_len);
            agent->cached_static_prompt_cap = agent->cached_static_prompt_len;
        }
        return sc_sprintf(agent->alloc, "Model: %.*s", (int)agent->model_name_len,
                          agent->model_name);
    }

    if (sc_strncasecmp(cmd_buf, "status", 6) == 0) {
        const char *prov = agent->provider.vtable->get_name(agent->provider.ctx);
        return sc_sprintf(agent->alloc,
                          "Provider: %s | Model: %.*s | History: %zu messages | Tokens: %llu",
                          prov ? prov : "?", (int)agent->model_name_len, agent->model_name,
                          (size_t)agent->history_count, (unsigned long long)agent->total_tokens);
    }

    if (sc_strncasecmp(cmd_buf, "cost", 4) == 0) {
        return sc_sprintf(agent->alloc,
                          "Tokens used: %llu (est. cost depends on provider pricing)\n"
                          "History: %zu messages",
                          (unsigned long long)agent->total_tokens, (size_t)agent->history_count);
    }

    if (sc_strncasecmp(cmd_buf, "provider", 8) == 0) {
        const char *prov = agent->provider.vtable->get_name(agent->provider.ctx);
        if (arg_len > 0) {
            return sc_sprintf(agent->alloc,
                              "Provider switching requires restart. Current: %s\n"
                              "Set in config: default_provider = \"%.*s\"",
                              prov ? prov : "?", (int)arg_len, arg_buf);
        }
        return sc_sprintf(agent->alloc, "Provider: %s", prov ? prov : "?");
    }

    if (sc_strncasecmp(cmd_buf, "tools", 5) == 0) {
        char *buf = (char *)agent->alloc->alloc(agent->alloc->ctx, 4096);
        if (!buf)
            return NULL;
        int off = snprintf(buf, 4096, "Tools (%zu):\n", agent->tools_count);
        if (off < 0 || (size_t)off >= 4096)
            off = 0;
        for (size_t i = 0; i < agent->tools_count && off < 4000; i++) {
            const char *n = agent->tools[i].vtable->name(agent->tools[i].ctx);
            int nw = snprintf(buf + off, 4096 - (size_t)off, "  %s\n", n ? n : "?");
            if (nw < 0)
                break;
            off += nw;
        }
        return buf;
    }

    if (sc_strncasecmp(cmd_buf, "plan", 4) == 0) {
        if (arg_len == 0) {
            return sc_strndup(agent->alloc,
                              "Usage: /plan {\"steps\": [{\"tool\": \"name\", \"args\": {...}}]}",
                              57);
        }
        char *summary = NULL;
        size_t summary_len = 0;
        sc_error_t err = sc_agent_execute_plan(agent, arg_buf, arg_len, &summary, &summary_len);
        if (err != SC_OK) {
            return sc_sprintf(agent->alloc, "Plan failed: %s", sc_error_string(err));
        }
        return summary;
    }

    if (sc_strncasecmp(cmd_buf, "goal", 4) == 0) {
        if (arg_len == 0) {
            return sc_strndup(agent->alloc,
                              "Usage: /goal <describe what you want to accomplish>", 51);
        }
        const char **tool_names = NULL;
        size_t tn_count = 0;
        if (agent->tools_count > 0) {
            tool_names =
                (const char **)agent->alloc->alloc(agent->alloc->ctx,
                                                   agent->tools_count * sizeof(const char *));
            if (tool_names) {
                for (size_t i = 0; i < agent->tools_count; i++) {
                    const char *tn =
                        agent->tools[i].vtable->name ? agent->tools[i].vtable->name(agent->tools[i].ctx) : NULL;
                    if (tn)
                        tool_names[tn_count++] = tn;
                }
            }
        }
        sc_plan_t *plan = NULL;
        sc_error_t err = sc_planner_generate(agent->alloc, &agent->provider, agent->model_name,
                                             agent->model_name_len, arg_buf, arg_len, tool_names,
                                             tn_count, &plan);
        if (tool_names)
            agent->alloc->free(agent->alloc->ctx, (void *)tool_names,
                               agent->tools_count * sizeof(const char *));
        if (err != SC_OK || !plan) {
            return sc_sprintf(agent->alloc, "Goal planning failed: %s", sc_error_string(err));
        }
        char *summary = sc_sprintf(agent->alloc, "Plan generated with %zu steps.",
                                    plan->steps_count);
        sc_plan_free(agent->alloc, plan);
        return summary;
    }

    if (sc_strncasecmp(cmd_buf, "spawn", 5) == 0) {
        if (!agent->agent_pool)
            return sc_strndup(agent->alloc, "Agent pool not configured.", 26);
        if (arg_len == 0)
            return sc_strndup(agent->alloc, "Usage: /spawn <task description>", 32);
        sc_spawn_config_t scfg;
        memset(&scfg, 0, sizeof(scfg));
        scfg.mode = SC_SPAWN_ONE_SHOT;
        scfg.max_iterations = 10;
        scfg.mailbox = agent->mailbox;
        if (agent->persona_name) {
            scfg.persona_name = agent->persona_name;
            scfg.persona_name_len = agent->persona_name_len;
        }
        uint64_t new_id = 0;
        sc_error_t err =
            sc_agent_pool_spawn(agent->agent_pool, &scfg, arg_buf, arg_len, "cli-spawn", &new_id);
        if (err != SC_OK)
            return sc_sprintf(agent->alloc, "Spawn failed: %s", sc_error_string(err));
        return sc_sprintf(agent->alloc, "Spawned agent #%llu", (unsigned long long)new_id);
    }

    if (sc_strncasecmp(cmd_buf, "agents", 6) == 0) {
        if (!agent->agent_pool)
            return sc_strndup(agent->alloc, "Agent pool not configured.", 26);
        sc_agent_pool_info_t *info = NULL;
        size_t info_count = 0;
        sc_error_t err = sc_agent_pool_list(agent->agent_pool, agent->alloc, &info, &info_count);
        if (err != SC_OK)
            return sc_sprintf(agent->alloc, "List failed: %s", sc_error_string(err));
        if (info_count == 0) {
            if (info)
                agent->alloc->free(agent->alloc->ctx, info, 0);
            return sc_strndup(agent->alloc, "No agents running.", 18);
        }
        char *buf = (char *)agent->alloc->alloc(agent->alloc->ctx, 4096);
        if (!buf) {
            agent->alloc->free(agent->alloc->ctx, info, info_count * sizeof(sc_agent_pool_info_t));
            return NULL;
        }
        int off = snprintf(buf, 4096, "Agents (%zu):\n", info_count);
        for (size_t i = 0; i < info_count && off < 4000; i++) {
            off += snprintf(buf + off, 4096 - (size_t)off, "  #%llu [%s] %s\n",
                            (unsigned long long)info[i].agent_id,
                            info[i].status == SC_AGENT_RUNNING     ? "running"
                            : info[i].status == SC_AGENT_IDLE      ? "idle"
                            : info[i].status == SC_AGENT_COMPLETED ? "done"
                            : info[i].status == SC_AGENT_FAILED    ? "failed"
                                                                   : "cancelled",
                            info[i].label ? info[i].label : "");
        }
        agent->alloc->free(agent->alloc->ctx, info, info_count * sizeof(sc_agent_pool_info_t));
        return buf;
    }

    if (sc_strncasecmp(cmd_buf, "cancel", 6) == 0) {
        if (!agent->agent_pool)
            return sc_strndup(agent->alloc, "Agent pool not configured.", 26);
        if (arg_len == 0)
            return sc_strndup(agent->alloc, "Usage: /cancel <agent-id>", 25);
        uint64_t cid = (uint64_t)strtoull(arg_buf, NULL, 10);
        sc_error_t err = sc_agent_pool_cancel(agent->agent_pool, cid);
        if (err != SC_OK)
            return sc_sprintf(agent->alloc, "Cancel failed: %s", sc_error_string(err));
        return sc_sprintf(agent->alloc, "Cancelled agent #%llu", (unsigned long long)cid);
    }

    if (sc_strncasecmp(cmd_buf, "send", 4) == 0) {
        if (!agent->mailbox)
            return sc_strndup(agent->alloc, "Mailbox not configured.", 22);
        if (arg_len == 0)
            return sc_strndup(agent->alloc, "Usage: /send <agent-id> <message>", 32);
        const char *p = arg_buf;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return sc_strndup(agent->alloc, "Usage: /send <agent-id> <message>", 32);
        uint64_t to_id = (uint64_t)strtoull(p, (char **)&p, 10);
        while (*p == ' ' || *p == '\t')
            p++;
        uint64_t from_id = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
        sc_error_t err =
            sc_mailbox_send(agent->mailbox, from_id, to_id, SC_MSG_TASK, p, strlen(p), 0);
        if (err != SC_OK)
            return sc_sprintf(agent->alloc, "Send failed: %s", sc_error_string(err));
        return sc_sprintf(agent->alloc, "Sent to agent #%llu", (unsigned long long)to_id);
    }

    if (sc_strncasecmp(cmd_buf, "tasks", 5) == 0) {
        if (!agent->task_list)
            return sc_strndup(agent->alloc, "Task list not configured.", 25);
        size_t pending = sc_task_list_count_by_status(agent->task_list, SC_TASK_LIST_PENDING);
        size_t claimed = sc_task_list_count_by_status(agent->task_list, SC_TASK_LIST_CLAIMED);
        size_t completed = sc_task_list_count_by_status(agent->task_list, SC_TASK_LIST_COMPLETED);
        return sc_sprintf(agent->alloc, "Tasks: %zu pending, %zu claimed, %zu completed", pending,
                          claimed, completed);
    }

    if (sc_strncasecmp(cmd_buf, "task", 4) == 0) {
        if (!agent->task_list)
            return sc_strndup(agent->alloc, "Task list not configured.", 25);
        const char *p = arg_buf;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return sc_strndup(agent->alloc, "Usage: /task add <subject> | claim <id> | done <id>",
                              52);
        if (sc_strncasecmp(p, "add", 3) == 0) {
            p += 3;
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p)
                return sc_strndup(agent->alloc, "Usage: /task add <subject>", 26);
            uint64_t id = 0;
            sc_error_t err = sc_task_list_add(agent->task_list, p, NULL, NULL, 0, &id);
            if (err != SC_OK)
                return sc_sprintf(agent->alloc, "Add failed: %s", sc_error_string(err));
            return sc_sprintf(agent->alloc, "Task #%llu created", (unsigned long long)id);
        }
        if (sc_strncasecmp(p, "claim", 5) == 0) {
            p += 5;
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p)
                return sc_strndup(agent->alloc, "Usage: /task claim <id>", 23);
            uint64_t tid = (uint64_t)strtoull(p, NULL, 10);
            uint64_t aid = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
            sc_error_t err = sc_task_list_claim(agent->task_list, tid, aid);
            if (err != SC_OK)
                return sc_sprintf(agent->alloc, "Claim failed: %s", sc_error_string(err));
            return sc_sprintf(agent->alloc, "Claimed task #%llu", (unsigned long long)tid);
        }
        if (sc_strncasecmp(p, "done", 4) == 0) {
            p += 4;
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p)
                return sc_strndup(agent->alloc, "Usage: /task done <id>", 22);
            uint64_t tid = (uint64_t)strtoull(p, NULL, 10);
            sc_error_t err =
                sc_task_list_update_status(agent->task_list, tid, SC_TASK_LIST_COMPLETED);
            if (err != SC_OK)
                return sc_sprintf(agent->alloc, "Done failed: %s", sc_error_string(err));
            return sc_sprintf(agent->alloc, "Task #%llu completed", (unsigned long long)tid);
        }
        return sc_strndup(agent->alloc, "Usage: /task add <subject> | claim <id> | done <id>", 52);
    }

    if (sc_strncasecmp(cmd_buf, "undo", 4) == 0) {
        if (!agent->undo_stack)
            return sc_strndup(agent->alloc, "Undo stack not configured.", 27);
        sc_error_t err = sc_undo_stack_execute_undo(agent->undo_stack, agent->alloc);
        if (err != SC_OK)
            return sc_sprintf(agent->alloc, "Undo failed: %s", sc_error_string(err));
        return sc_strndup(agent->alloc, "Undone.", 7);
    }

    return NULL;
}

static sc_policy_action_t sc_agent_check_policy(sc_agent_t *agent, const char *tool_name,
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

static sc_policy_action_t evaluate_tool_policy(sc_agent_t *agent, const char *tool_name,
                                               const char *args_json) {
    sc_policy_action_t base = sc_agent_check_policy(agent, tool_name, args_json);
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

static sc_tool_t *find_tool(sc_agent_t *agent, const char *name, size_t name_len) {
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
    agent_record_cost(agent, &resp.usage);
    sc_chat_response_free(agent->alloc, &resp);
    return SC_OK;
}

static void agent_maybe_tts(sc_agent_t *agent, const char *text, size_t text_len) {
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

static void process_mailbox_messages(sc_agent_t *agent) {
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
            (void)append_history(agent, SC_ROLE_USER, buf, (size_t)n, NULL, 0, NULL, 0);
        sc_message_free(agent->alloc, &msg);
    }
}

sc_error_t sc_agent_turn(sc_agent_t *agent, const char *msg, size_t msg_len, char **response_out,
                         size_t *response_len_out) {
    if (!agent || !msg || !response_out)
        return SC_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    sc_agent_set_current_for_tools(agent);

    process_mailbox_messages(agent);

    char *slash_resp = sc_agent_handle_slash_command(agent, msg, msg_len);
    if (slash_resp) {
        sc_agent_clear_current_for_tools();
        *response_out = slash_resp;
        if (response_len_out)
            *response_len_out = strlen(slash_resp);
        return SC_OK;
    }

    sc_error_t err = append_history(agent, SC_ROLE_USER, msg, msg_len, NULL, 0, NULL, 0);
    if (err != SC_OK) {
        sc_agent_clear_current_for_tools();
        return err;
    }

    /* Detect preferences from user corrections and store them */
    bool is_correction = sc_preferences_is_correction(msg, msg_len);
    if (agent->memory && is_correction) {
        size_t pref_len = 0;
        char *pref = sc_preferences_extract(agent->alloc, msg, msg_len, &pref_len);
        if (pref) {
            sc_preferences_store(agent->memory, agent->alloc, pref, pref_len);
            agent->alloc->free(agent->alloc->ctx, pref, pref_len + 1);
        }
    }

    /* Outcome tracking: record corrections and positive feedback */
    if (agent->outcomes) {
        if (is_correction) {
            const char *prev_response = NULL;
            if (agent->history_count >= 2 &&
                agent->history[agent->history_count - 2].role == SC_ROLE_ASSISTANT)
                prev_response = agent->history[agent->history_count - 2].content;
            sc_outcome_record_correction(agent->outcomes, prev_response, msg);

#ifdef SC_HAS_PERSONA
            if (agent->outcomes->auto_apply_feedback && agent->persona && agent->persona_name &&
                prev_response) {
                sc_persona_feedback_t fb = {
                    .channel = agent->active_channel,
                    .channel_len = agent->active_channel_len,
                    .original_response = prev_response,
                    .original_response_len = strlen(prev_response),
                    .corrected_response = msg,
                    .corrected_response_len = msg_len,
                };
                (void)sc_persona_feedback_record(agent->alloc, agent->persona_name,
                                                 strlen(agent->persona_name), &fb);
            }
#endif
        } else if (msg_len >= 5 && msg_len <= 80) {
            /* Detect simple positive feedback */
            bool positive = false;
            for (size_t k = 0; k + 5 <= msg_len && !positive; k++) {
                char c0 = msg[k] | 0x20, c1 = msg[k + 1] | 0x20, c2 = msg[k + 2] | 0x20;
                char c3 = msg[k + 3] | 0x20, c4 = msg[k + 4] | 0x20;
                if (c0 == 't' && c1 == 'h' && c2 == 'a' && c3 == 'n' && c4 == 'k')
                    positive = true;
                if (c0 == 'g' && c1 == 'r' && c2 == 'e' && c3 == 'a' && c4 == 't')
                    positive = true;
                if (k + 6 <= msg_len && c0 == 'p' && c1 == 'e' && c2 == 'r' && c3 == 'f' &&
                    c4 == 'e' && (msg[k + 5] | 0x20) == 'c')
                    positive = true;
            }
            if (positive)
                sc_outcome_record_positive(agent->outcomes, msg);
        }
    }

    /* Detect tone from recent user messages */
    const char *tone_hint = NULL;
    size_t tone_hint_len = 0;
    {
        const char *recent_msgs[3];
        size_t recent_lens[3];
        size_t rm_count = 0;
        for (size_t i = agent->history_count; i > 0 && rm_count < 3; i--) {
            if (agent->history[i - 1].role == SC_ROLE_USER && agent->history[i - 1].content) {
                recent_msgs[rm_count] = agent->history[i - 1].content;
                recent_lens[rm_count] = agent->history[i - 1].content_len;
                rm_count++;
            }
        }
        if (rm_count > 0) {
            sc_tone_t tone = sc_detect_tone(recent_msgs, recent_lens, rm_count);
            tone_hint = sc_tone_hint_string(tone, &tone_hint_len);
        }
    }

    /* Load user preferences for prompt injection */
    char *pref_ctx = NULL;
    size_t pref_ctx_len = 0;
    if (agent->memory)
        (void)sc_preferences_load(agent->memory, agent->alloc, &pref_ctx, &pref_ctx_len);

    /* Load memory context for this turn */
    char *memory_ctx = NULL;
    size_t memory_ctx_len = 0;
    if (agent->memory && agent->memory->vtable) {
        sc_memory_loader_t loader;
        sc_memory_loader_init(&loader, agent->alloc, agent->memory, agent->retrieval_engine, 10,
                              4000);
        (void)sc_memory_loader_load(&loader, msg, msg_len, "", 0, &memory_ctx, &memory_ctx_len);
    }

    /* Build situational awareness context */
    char *awareness_ctx = NULL;
    size_t awareness_ctx_len = 0;
    if (agent->awareness)
        awareness_ctx = sc_awareness_context(agent->awareness, agent->alloc, &awareness_ctx_len);

    /* Build persona prompt fresh each turn (channel-dependent; no caching) */
    char *persona_prompt = NULL;
    size_t persona_prompt_len = 0;
#ifdef SC_HAS_PERSONA
    if (agent->persona) {
        const char *ch = agent->active_channel;
        size_t ch_len = agent->active_channel_len;
        sc_error_t perr = sc_persona_build_prompt(agent->alloc, agent->persona, ch, ch_len,
                                                  &persona_prompt, &persona_prompt_len);
        if (perr != SC_OK) {
            if (pref_ctx)
                agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
            if (memory_ctx)
                agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
            if (awareness_ctx)
                agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
            sc_agent_clear_current_for_tools();
            return perr;
        }
    }
#endif

    /* Build system prompt using cached static portion when available */
    char *system_prompt = NULL;
    size_t system_prompt_len = 0;
    if (agent->cached_static_prompt && !pref_ctx && !tone_hint && !persona_prompt &&
        !awareness_ctx) {
        err = sc_prompt_build_with_cache(agent->alloc, agent->cached_static_prompt,
                                         agent->cached_static_prompt_len, memory_ctx,
                                         memory_ctx_len, &system_prompt, &system_prompt_len);
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (err != SC_OK) {
            if (pref_ctx)
                agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
            sc_agent_clear_current_for_tools();
            return err;
        }
    } else {
        sc_prompt_config_t cfg = {
            .provider_name = agent->provider.vtable->get_name(agent->provider.ctx),
            .provider_name_len = 0,
            .model_name = agent->model_name,
            .model_name_len = agent->model_name_len,
            .workspace_dir = agent->workspace_dir,
            .workspace_dir_len = agent->workspace_dir_len,
            .tools = agent->tools,
            .tools_count = agent->tools_count,
            .memory_context = memory_ctx,
            .memory_context_len = memory_ctx_len,
            .autonomy_level = agent->autonomy_level,
            .custom_instructions = agent->custom_instructions,
            .custom_instructions_len = agent->custom_instructions_len,
            .persona_prompt = persona_prompt,
            .persona_prompt_len = persona_prompt_len,
            .preferences = pref_ctx,
            .preferences_len = pref_ctx_len,
            .chain_of_thought = agent->chain_of_thought,
            .tone_hint = tone_hint,
            .tone_hint_len = tone_hint_len,
            .awareness_context = awareness_ctx,
            .awareness_context_len = awareness_ctx_len,
        };
        err = sc_prompt_build_system(agent->alloc, &cfg, &system_prompt, &system_prompt_len);
        if (persona_prompt)
            agent->alloc->free(agent->alloc->ctx, persona_prompt, persona_prompt_len + 1);
        persona_prompt = NULL;
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (awareness_ctx)
            agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
        if (err != SC_OK) {
            if (pref_ctx)
                agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
            sc_agent_clear_current_for_tools();
            return err;
        }
    }
    if (pref_ctx)
        agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);

    sc_chat_message_t *msgs = NULL;
    size_t msgs_count = 0;

    sc_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.model = agent->model_name;
    req.model_len = agent->model_name_len;
    req.temperature = agent->temperature;
    req.tools = (agent->tool_specs_count > 0) ? agent->tool_specs : NULL;
    req.tools_count = agent->tool_specs_count;

    uint32_t iter = 0;
    int reflection_retries_left = agent->reflection.max_retries;
    uint64_t max_tokens =
        agent->token_limit ? agent->token_limit
                           : sc_context_tokens_resolve(0, agent->model_name, agent->model_name_len);
    if (max_tokens == 0)
        max_tokens = 128000u;

    sc_compaction_config_t compact_cfg;
    sc_compaction_config_default(&compact_cfg);
    compact_cfg.max_history_messages = agent->max_history_messages;
    compact_cfg.token_limit = max_tokens;

    clock_t turn_start = clock();
    uint64_t turn_tokens = 0;
    const char *prov_name = agent->provider.vtable->get_name
                                ? agent->provider.vtable->get_name(agent->provider.ctx)
                                : NULL;

    {
        sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_AGENT_START, .data = {{0}}};
        ev.data.agent_start.provider = prov_name ? prov_name : "";
        ev.data.agent_start.model = agent->model_name ? agent->model_name : "";
        SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }

    /* Per-turn arena: reset each iteration to reclaim ephemeral message arrays */
    sc_allocator_t turn_alloc =
        agent->turn_arena ? sc_arena_allocator(agent->turn_arena) : *agent->alloc;

    while (iter < agent->max_tool_iterations) {
        if (agent->cancel_requested) {
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                sc_arena_reset(agent->turn_arena);
            return SC_ERR_CANCELLED;
        }
        if (agent->turn_arena)
            sc_arena_reset(agent->turn_arena);
        msgs = NULL;
        msgs_count = 0;
        iter++;
        /* Compact history if it exceeds limits (before each provider call).
         * Uses LLM summarization when the provider is available, with
         * rule-based fallback. */
        if (sc_should_compact(agent->history, agent->history_count, &compact_cfg)) {
            sc_compact_history_llm(agent->alloc, agent->history, &agent->history_count,
                                   &agent->history_cap, &compact_cfg, &agent->provider);
        }

        /* Context pressure: estimate tokens, check thresholds, auto-compact if needed */
        {
            uint64_t current = sc_estimate_tokens(agent->history, agent->history_count) +
                               (uint64_t)((system_prompt_len + 3) / 4);
            sc_context_pressure_t pr = {
                .current_tokens = (size_t)current,
                .max_tokens = (size_t)max_tokens,
                .pressure = 0.0f,
                .warning_85_emitted = agent->context_pressure_warning_85_emitted,
                .warning_95_emitted = agent->context_pressure_warning_95_emitted,
            };
            if (sc_context_check_pressure(&pr, agent->context_pressure_warn,
                                          agent->context_pressure_compact)) {
                sc_context_compact_for_pressure(agent->alloc, agent->history, &agent->history_count,
                                                &agent->history_cap, (size_t)max_tokens,
                                                agent->context_compact_target);
                agent->context_pressure_warning_85_emitted = false;
                agent->context_pressure_warning_95_emitted = false;
            } else {
                agent->context_pressure_warning_85_emitted = pr.warning_85_emitted;
                agent->context_pressure_warning_95_emitted = pr.warning_95_emitted;
            }
        }

        /* Format messages for this iteration using arena allocator */
        {
            sc_chat_message_t *hist_msgs = NULL;
            size_t hist_count = 0;
            err = sc_context_format_messages(&turn_alloc, agent->history, agent->history_count,
                                             agent->max_history_messages, &hist_msgs, &hist_count);
            if (err != SC_OK) {
                sc_agent_clear_current_for_tools();
                if (system_prompt)
                    agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                return err;
            }
            size_t total = (hist_msgs ? hist_count : 0) + 1;
            sc_chat_message_t *all = (sc_chat_message_t *)turn_alloc.alloc(
                turn_alloc.ctx, total * sizeof(sc_chat_message_t));
            if (!all) {
                sc_agent_clear_current_for_tools();
                if (system_prompt)
                    agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                return SC_ERR_OUT_OF_MEMORY;
            }
            all[0].role = SC_ROLE_SYSTEM;
            all[0].content = system_prompt;
            all[0].content_len = system_prompt_len;
            all[0].name = NULL;
            all[0].name_len = 0;
            all[0].tool_call_id = NULL;
            all[0].tool_call_id_len = 0;
            all[0].content_parts = NULL;
            all[0].content_parts_count = 0;
            for (size_t i = 0; i < (hist_msgs ? hist_count : 0); i++)
                all[i + 1] = hist_msgs[i];
            msgs = all;
            msgs_count = total;
            req.messages = msgs;
            req.messages_count = msgs_count;
        }

        {
            sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_LLM_REQUEST, .data = {{0}}};
            ev.data.llm_request.provider = prov_name ? prov_name : "";
            ev.data.llm_request.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_request.messages_count = msgs_count;
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        clock_t llm_start = clock();
        sc_chat_response_t resp;
        memset(&resp, 0, sizeof(resp));
        err =
            agent->provider.vtable->chat(agent->provider.ctx, agent->alloc, &req, agent->model_name,
                                         agent->model_name_len, agent->temperature, &resp);
        uint64_t llm_duration_ms = clock_diff_ms(llm_start, clock());

        {
            sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}}};
            ev.data.llm_response.provider = prov_name ? prov_name : "";
            ev.data.llm_response.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_response.duration_ms = llm_duration_ms;
            ev.data.llm_response.success = (err == SC_OK);
            ev.data.llm_response.error_message = (err != SC_OK) ? "chat failed" : NULL;
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        if (err != SC_OK) {
            {
                sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_ERR, .data = {{0}}};
                ev.data.err.component = "agent";
                ev.data.err.message = "provider chat failed";
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            sc_agent_clear_current_for_tools();
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                sc_arena_reset(agent->turn_arena);
            return err;
        }

        agent->total_tokens += resp.usage.total_tokens;
        agent_record_cost(agent, &resp.usage);
        turn_tokens += resp.usage.total_tokens;

        if (resp.tool_calls_count == 0) {
            uint64_t turn_duration_ms = clock_diff_ms(turn_start, clock());
            {
                sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_AGENT_END, .data = {{0}}};
                ev.data.agent_end.duration_ms = turn_duration_ms;
                ev.data.agent_end.tokens_used = turn_tokens;
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            {
                sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TURN_COMPLETE, .data = {{0}}};
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            if (resp.content && resp.content_len > 0) {
                /* Reflection: evaluate response quality and retry if needed */
                sc_reflection_quality_t quality = sc_reflection_evaluate(
                    msg, msg_len, resp.content, resp.content_len, &agent->reflection);

                if (quality == SC_QUALITY_NEEDS_RETRY && agent->reflection.enabled &&
                    reflection_retries_left > 0 && iter < agent->max_tool_iterations - 1) {
                    reflection_retries_left--;
                    char *critique = NULL;
                    size_t critique_len = 0;
                    sc_error_t cerr = sc_reflection_build_critique_prompt(
                        agent->alloc, msg, msg_len, resp.content, resp.content_len, &critique,
                        &critique_len);
                    if (cerr == SC_OK && critique) {
                        (void)append_history(agent, SC_ROLE_ASSISTANT, resp.content,
                                             resp.content_len, NULL, 0, NULL, 0);
                        (void)append_history(agent, SC_ROLE_USER, critique, critique_len, NULL, 0,
                                             NULL, 0);
                        agent->alloc->free(agent->alloc->ctx, critique, critique_len + 1);
                        sc_chat_response_free(agent->alloc, &resp);
                        iter++;
                        continue; /* retry with critique feedback */
                    }
                    if (critique)
                        agent->alloc->free(agent->alloc->ctx, critique, critique_len + 1);
                }

                (void)append_history(agent, SC_ROLE_ASSISTANT, resp.content, resp.content_len, NULL,
                                     0, NULL, 0);
                *response_out = sc_strndup(agent->alloc, resp.content, resp.content_len);
                if (!*response_out) {
                    sc_agent_clear_current_for_tools();
                    sc_chat_response_free(agent->alloc, &resp);
                    if (system_prompt)
                        agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                    if (agent->turn_arena)
                        sc_arena_reset(agent->turn_arena);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                if (response_len_out)
                    *response_len_out = resp.content_len;
                agent_maybe_tts(agent, resp.content, resp.content_len);
            }
            sc_chat_response_free(agent->alloc, &resp);
            sc_agent_clear_current_for_tools();
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                sc_arena_reset(agent->turn_arena);
            return SC_OK;
        }

        err = append_history_with_tool_calls(agent, resp.content ? resp.content : "",
                                             resp.content_len, resp.tool_calls,
                                             resp.tool_calls_count);
        if (err != SC_OK) {
            sc_agent_clear_current_for_tools();
            sc_chat_response_free(agent->alloc, &resp);
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                sc_arena_reset(agent->turn_arena);
            return err;
        }
        sc_chat_response_free(agent->alloc, &resp);

        {
            size_t tc_count = agent->history[agent->history_count - 1].tool_calls_count;
            const sc_tool_call_t *calls = agent->history[agent->history_count - 1].tool_calls;

            /* Emit TOOL_CALL_START events for all calls */
            for (size_t tc = 0; tc < tc_count; tc++) {
                char tn_buf[64];
                size_t tn = (calls[tc].name_len < sizeof(tn_buf) - 1) ? calls[tc].name_len
                                                                      : sizeof(tn_buf) - 1;
                if (tn > 0 && calls[tc].name)
                    memcpy(tn_buf, calls[tc].name, tn);
                tn_buf[tn] = '\0';
                sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}}};
                ev.data.tool_call_start.tool = tn_buf[0] ? tn_buf : "unknown";
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }

            /* LOCKED: skip all tool execution */
            if (agent->autonomy_level == SC_AUTONOMY_LOCKED) {
                for (size_t tc = 0; tc < tc_count; tc++) {
                    const sc_tool_call_t *call = &calls[tc];
                    (void)append_history(agent, SC_ROLE_TOOL,
                                         "Action blocked: agent is in locked mode", 38, call->name,
                                         call->name_len, call->id, call->id_len);
                    if (agent->cancel_requested)
                        break;
                }
            } else {
                /* Use dispatcher for parallel execution when enabled (Tier 1.3) */
                sc_dispatcher_t dispatcher;
                sc_dispatcher_default(&dispatcher);
                if (tc_count > 1)
                    dispatcher.max_parallel = 4;
                dispatcher.timeout_secs = 30;

                sc_dispatch_result_t dispatch_result;
                memset(&dispatch_result, 0, sizeof(dispatch_result));
                err = sc_dispatcher_dispatch(&dispatcher, agent->alloc, agent->tools,
                                             agent->tools_count, calls, tc_count, &dispatch_result);

                if (err == SC_OK && dispatch_result.results) {
                    for (size_t tc = 0; tc < tc_count; tc++) {
                        const sc_tool_call_t *call = &calls[tc];
                        sc_tool_result_t *result = &dispatch_result.results[tc];

                        char tn_buf[64];
                        size_t tn = (call->name_len < sizeof(tn_buf) - 1) ? call->name_len
                                                                          : sizeof(tn_buf) - 1;
                        if (tn > 0 && call->name)
                            memcpy(tn_buf, call->name, tn);
                        tn_buf[tn] = '\0';
                        const char *args_str = call->arguments ? call->arguments : "";

                        /* Policy evaluation (dispatcher path) */
                        sc_policy_action_t pa =
                            evaluate_tool_policy(agent, tn_buf[0] ? tn_buf : "unknown", args_str);
                        if (pa == SC_POLICY_DENY) {
                            if (agent->audit_logger) {
                                sc_audit_event_t aev;
                                sc_audit_event_init(&aev, SC_AUDIT_POLICY_VIOLATION);
                                sc_audit_event_with_identity(
                                    &aev, agent->agent_id,
                                    agent->model_name ? agent->model_name : "unknown", NULL);
                                sc_audit_event_with_action(&aev, tn_buf[0] ? tn_buf : "unknown",
                                                           "denied", false, false);
                                sc_audit_logger_log(agent->audit_logger, &aev);
                            }
                            sc_tool_result_free(agent->alloc, result);
                            *result = sc_tool_result_fail("denied by policy", 16);
                        } else if (pa == SC_POLICY_REQUIRE_APPROVAL) {
                            result->needs_approval = true;
                        }

                        /* Autonomy: SUPERVISED forces approval; ASSISTED for medium/high risk */
                        if (agent->autonomy_level == SC_AUTONOMY_SUPERVISED) {
                            result->needs_approval = true;
                        } else if (agent->autonomy_level == SC_AUTONOMY_ASSISTED) {
                            if (sc_tool_risk_level(tn_buf[0] ? tn_buf : "unknown") >=
                                SC_RISK_MEDIUM)
                                result->needs_approval = true;
                        }

                        /* Feature 2: explicit failure when approval required but no callback */
                        if (result->needs_approval && !agent->approval_cb) {
                            sc_tool_result_free(agent->alloc, result);
                            *result = sc_tool_result_fail("requires human approval", 23);
                        }

                        /* Approval flow: if tool needs approval, ask user and retry */
                        if (result->needs_approval && agent->approval_cb) {
                            char tn_tmp[64];
                            size_t tn2 = (call->name_len < sizeof(tn_tmp) - 1) ? call->name_len
                                                                               : sizeof(tn_tmp) - 1;
                            if (tn2 > 0 && call->name)
                                memcpy(tn_tmp, call->name, tn2);
                            tn_tmp[tn2] = '\0';
                            bool user_approved =
                                agent->approval_cb(agent->approval_ctx, tn_tmp, args_str);
                            if (user_approved) {
                                sc_tool_result_free(agent->alloc, result);
                                if (agent->policy)
                                    agent->policy->pre_approved = true;
                                sc_tool_t *tool = find_tool(agent, call->name, call->name_len);
                                if (tool) {
                                    sc_json_value_t *retry_args = NULL;
                                    if (call->arguments_len > 0)
                                        (void)sc_json_parse(agent->alloc, call->arguments,
                                                            call->arguments_len, &retry_args);
                                    *result = sc_tool_result_fail("invalid arguments", 16);
                                    if (retry_args) {
                                        if (tool->vtable->execute)
                                            tool->vtable->execute(tool->ctx, agent->alloc,
                                                                  retry_args, result);
                                        sc_json_free(agent->alloc, retry_args);
                                    }
                                }
                            } else {
                                sc_tool_result_free(agent->alloc, result);
                                *result = sc_tool_result_fail("user denied action", 18);
                            }
                        }

                        {
                            sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_CALL,
                                                      .data = {{0}}};
                            ev.data.tool_call.tool = tn_buf[0] ? tn_buf : "unknown";
                            ev.data.tool_call.duration_ms = 0;
                            ev.data.tool_call.success = result->success;
                            ev.data.tool_call.detail =
                                result->success
                                    ? NULL
                                    : (result->error_msg ? result->error_msg : "failed");
                            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
                        }

                        /* Outcome tracking */
                        if (agent->outcomes) {
                            const char *sum =
                                result->success
                                    ? (result->output ? result->output : "ok")
                                    : (result->error_msg ? result->error_msg : "failed");
                            sc_outcome_record_tool(agent->outcomes, tn_buf, result->success, sum);
                        }

                        const char *res_content =
                            result->success ? result->output : result->error_msg;
                        size_t res_len =
                            result->success ? result->output_len : result->error_msg_len;
                        (void)append_history(agent, SC_ROLE_TOOL, res_content, res_len, call->name,
                                             call->name_len, call->id, call->id_len);

                        if (agent->audit_logger) {
                            sc_audit_event_t aev;
                            sc_audit_event_init(&aev, SC_AUDIT_COMMAND_EXECUTION);
                            sc_audit_event_with_identity(
                                &aev, agent->agent_id,
                                agent->model_name ? agent->model_name : "unknown", NULL);
                            sc_audit_event_with_action(&aev, tn_buf, "tool", result->success, true);
                            sc_audit_event_with_result(&aev, result->success, 0, 0,
                                                       result->success ? NULL : result->error_msg);
                            sc_audit_logger_log(agent->audit_logger, &aev);
                        }

                        if (agent->cancel_requested)
                            break;
                    }
                    sc_dispatch_result_free(agent->alloc, &dispatch_result);
                } else {
                    /* Fallback: sequential if dispatcher fails */
                    for (size_t tc = 0; tc < tc_count; tc++) {
                        const sc_tool_call_t *call = &calls[tc];
                        sc_tool_t *tool = find_tool(agent, call->name, call->name_len);
                        if (!tool) {
                            (void)append_history(agent, SC_ROLE_TOOL, "tool not found", 14,
                                                 call->name, call->name_len, call->id,
                                                 call->id_len);
                            continue;
                        }
                        char pol_tn[64];
                        size_t pol_tn_len = call->name_len < sizeof(pol_tn) - 1
                                                ? call->name_len
                                                : sizeof(pol_tn) - 1;
                        if (pol_tn_len > 0 && call->name)
                            memcpy(pol_tn, call->name, pol_tn_len);
                        pol_tn[pol_tn_len] = '\0';

                        sc_policy_action_t pa = evaluate_tool_policy(
                            agent, pol_tn, call->arguments ? call->arguments : "");
                        bool force_approval = (agent->autonomy_level == SC_AUTONOMY_SUPERVISED) ||
                                              (agent->autonomy_level == SC_AUTONOMY_ASSISTED &&
                                               sc_tool_risk_level(pol_tn) >= SC_RISK_MEDIUM);

                        sc_tool_result_t result = sc_tool_result_fail("invalid arguments", 16);
                        if (pa == SC_POLICY_DENY) {
                            if (agent->audit_logger) {
                                sc_audit_event_t aev;
                                sc_audit_event_init(&aev, SC_AUDIT_POLICY_VIOLATION);
                                sc_audit_event_with_identity(
                                    &aev, agent->agent_id,
                                    agent->model_name ? agent->model_name : "unknown", NULL);
                                sc_audit_event_with_action(&aev, pol_tn, "denied", false, false);
                                sc_audit_logger_log(agent->audit_logger, &aev);
                            }
                            result = sc_tool_result_fail("denied by policy", 16);
                        } else if (pa == SC_POLICY_REQUIRE_APPROVAL || force_approval) {
                            result = sc_tool_result_fail("pending approval", 16);
                            result.needs_approval = true;
                        } else {
                            sc_json_value_t *args = NULL;
                            if (call->arguments_len > 0) {
                                sc_error_t pe = sc_json_parse(agent->alloc, call->arguments,
                                                              call->arguments_len, &args);
                                if (pe == SC_OK && args) {
                                    tool->vtable->execute(tool->ctx, agent->alloc, args, &result);
                                    sc_json_free(agent->alloc, args);
                                }
                            }
                        }

                        /* Feature 2: explicit failure when approval required but no callback */
                        if (result.needs_approval && !agent->approval_cb) {
                            sc_tool_result_free(agent->alloc, &result);
                            result = sc_tool_result_fail("requires human approval", 23);
                        }

                        /* Approval retry for sequential fallback path */
                        if (result.needs_approval && agent->approval_cb) {
                            char seq_tn[64];
                            size_t seq_n = (call->name_len < sizeof(seq_tn) - 1)
                                               ? call->name_len
                                               : sizeof(seq_tn) - 1;
                            if (seq_n > 0 && call->name)
                                memcpy(seq_tn, call->name, seq_n);
                            seq_tn[seq_n] = '\0';
                            if (agent->approval_cb(agent->approval_ctx, seq_tn,
                                                   call->arguments ? call->arguments : "")) {
                                sc_tool_result_free(agent->alloc, &result);
                                if (agent->policy)
                                    agent->policy->pre_approved = true;
                                sc_json_value_t *retry_args = NULL;
                                if (call->arguments_len > 0)
                                    (void)sc_json_parse(agent->alloc, call->arguments,
                                                        call->arguments_len, &retry_args);
                                result = sc_tool_result_fail("invalid arguments", 16);
                                if (retry_args) {
                                    tool->vtable->execute(tool->ctx, agent->alloc, retry_args,
                                                          &result);
                                    sc_json_free(agent->alloc, retry_args);
                                }
                            } else {
                                sc_tool_result_free(agent->alloc, &result);
                                result = sc_tool_result_fail("user denied action", 18);
                            }
                        }

                        const char *res_content = result.success ? result.output : result.error_msg;
                        size_t res_len = result.success ? result.output_len : result.error_msg_len;
                        (void)append_history(agent, SC_ROLE_TOOL, res_content, res_len, call->name,
                                             call->name_len, call->id, call->id_len);

                        if (agent->audit_logger) {
                            sc_audit_event_t aev;
                            sc_audit_event_init(&aev, SC_AUDIT_COMMAND_EXECUTION);
                            sc_audit_event_with_identity(
                                &aev, agent->agent_id,
                                agent->model_name ? agent->model_name : "unknown", NULL);
                            sc_audit_event_with_action(&aev, pol_tn, "tool", result.success, true);
                            sc_audit_event_with_result(&aev, result.success, 0, 0,
                                                       result.success ? NULL : result.error_msg);
                            sc_audit_logger_log(agent->audit_logger, &aev);
                        }

                        sc_tool_result_free(agent->alloc, &result);
                        if (agent->cancel_requested)
                            break;
                    }
                }
            }
        }
        /* Messages will be reformatted at top of next iteration via arena */
    }

    {
        sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED,
                                  .data = {{0}}};
        ev.data.tool_iterations_exhausted.iterations = agent->max_tool_iterations;
        SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }
    {
        sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_ERR, .data = {{0}}};
        ev.data.err.component = "agent";
        ev.data.err.message = "tool iterations exhausted";
        SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }
    sc_agent_clear_current_for_tools();
    if (system_prompt)
        agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
    if (agent->turn_arena)
        sc_arena_reset(agent->turn_arena);
    return SC_ERR_TIMEOUT;
}

typedef struct stream_token_wrap {
    sc_agent_stream_token_cb on_token;
    void *token_ctx;
} stream_token_wrap_t;

static void stream_chunk_to_token_cb(void *ctx, const sc_stream_chunk_t *chunk) {
    stream_token_wrap_t *w = (stream_token_wrap_t *)ctx;
    if (chunk->is_final || !w->on_token)
        return;
    if (chunk->delta && chunk->delta_len > 0)
        w->on_token(chunk->delta, chunk->delta_len, w->token_ctx);
}

sc_error_t sc_agent_turn_stream(sc_agent_t *agent, const char *msg, size_t msg_len,
                                sc_agent_stream_token_cb on_token, void *token_ctx,
                                char **response_out, size_t *response_len_out) {
    if (!agent || !msg || !response_out)
        return SC_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    sc_agent_set_current_for_tools(agent);

    process_mailbox_messages(agent);

    char *slash_resp = sc_agent_handle_slash_command(agent, msg, msg_len);
    if (slash_resp) {
        sc_agent_clear_current_for_tools();
        *response_out = slash_resp;
        if (response_len_out)
            *response_len_out = strlen(slash_resp);
        return SC_OK;
    }

    bool can_stream = (on_token != NULL) && agent->provider.vtable->supports_streaming &&
                      agent->provider.vtable->supports_streaming(agent->provider.ctx) &&
                      agent->provider.vtable->stream_chat;

    if (!can_stream) {
        sc_error_t fallback_err =
            sc_agent_turn(agent, msg, msg_len, response_out, response_len_out);
        if (fallback_err == SC_OK && on_token && *response_out && response_len_out &&
            *response_len_out > 0) {
            size_t chunk_size = 12;
            for (size_t i = 0; i < *response_len_out; i += chunk_size) {
                size_t n = *response_len_out - i;
                if (n > chunk_size)
                    n = chunk_size;
                on_token(*response_out + i, n, token_ctx);
            }
        }
        sc_agent_clear_current_for_tools();
        return fallback_err;
    }

    /* When tools are present, fall back to sc_agent_turn but simulate
     * streaming by chunking the final response to the callback.
     * This keeps the TUI responsive while tools execute. (Tier 2.4) */
    bool has_tools = (agent->tool_specs_count > 0);
    if (has_tools) {
        sc_error_t fallback_err =
            sc_agent_turn(agent, msg, msg_len, response_out, response_len_out);
        if (fallback_err == SC_OK && on_token && *response_out && response_len_out &&
            *response_len_out > 0) {
            size_t chunk_size = 8;
            for (size_t i = 0; i < *response_len_out; i += chunk_size) {
                size_t n = *response_len_out - i;
                if (n > chunk_size)
                    n = chunk_size;
                on_token(*response_out + i, n, token_ctx);
            }
        }
        sc_agent_clear_current_for_tools();
        return fallback_err;
    }

    sc_error_t err = append_history(agent, SC_ROLE_USER, msg, msg_len, NULL, 0, NULL, 0);
    if (err != SC_OK) {
        sc_agent_clear_current_for_tools();
        return err;
    }

    char *memory_ctx = NULL;
    size_t memory_ctx_len = 0;
    if (agent->memory && agent->memory->vtable) {
        sc_memory_loader_t loader;
        sc_memory_loader_init(&loader, agent->alloc, agent->memory, agent->retrieval_engine, 10,
                              4000);
        (void)sc_memory_loader_load(&loader, msg, msg_len, "", 0, &memory_ctx, &memory_ctx_len);
    }

    /* Build situational awareness context */
    char *awareness_ctx = NULL;
    size_t awareness_ctx_len = 0;
    if (agent->awareness)
        awareness_ctx = sc_awareness_context(agent->awareness, agent->alloc, &awareness_ctx_len);

    /* Build persona prompt fresh each turn (channel-dependent; no caching) */
    char *persona_prompt = NULL;
    size_t persona_prompt_len = 0;
#ifdef SC_HAS_PERSONA
    if (agent->persona) {
        const char *ch = agent->active_channel;
        size_t ch_len = agent->active_channel_len;
        sc_error_t perr = sc_persona_build_prompt(agent->alloc, agent->persona, ch, ch_len,
                                                  &persona_prompt, &persona_prompt_len);
        if (perr != SC_OK) {
            if (memory_ctx)
                agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
            if (awareness_ctx)
                agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
            sc_agent_clear_current_for_tools();
            return perr;
        }
    }
#endif

    char *system_prompt = NULL;
    size_t system_prompt_len = 0;
    if (agent->cached_static_prompt && !persona_prompt && !awareness_ctx) {
        err = sc_prompt_build_with_cache(agent->alloc, agent->cached_static_prompt,
                                         agent->cached_static_prompt_len, memory_ctx,
                                         memory_ctx_len, &system_prompt, &system_prompt_len);
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (err != SC_OK) {
            sc_agent_clear_current_for_tools();
            return err;
        }
    } else {
        sc_prompt_config_t cfg = {
            .provider_name = agent->provider.vtable->get_name(agent->provider.ctx),
            .provider_name_len = 0,
            .model_name = agent->model_name,
            .model_name_len = agent->model_name_len,
            .workspace_dir = agent->workspace_dir,
            .workspace_dir_len = agent->workspace_dir_len,
            .tools = agent->tools,
            .tools_count = agent->tools_count,
            .memory_context = memory_ctx,
            .memory_context_len = memory_ctx_len,
            .autonomy_level = agent->autonomy_level,
            .custom_instructions = agent->custom_instructions,
            .custom_instructions_len = agent->custom_instructions_len,
            .persona_prompt = persona_prompt,
            .persona_prompt_len = persona_prompt_len,
            .awareness_context = awareness_ctx,
            .awareness_context_len = awareness_ctx_len,
        };
        err = sc_prompt_build_system(agent->alloc, &cfg, &system_prompt, &system_prompt_len);
        if (persona_prompt)
            agent->alloc->free(agent->alloc->ctx, persona_prompt, persona_prompt_len + 1);
        persona_prompt = NULL;
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (awareness_ctx)
            agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
        if (err != SC_OK) {
            sc_agent_clear_current_for_tools();
            return err;
        }
    }

    sc_chat_message_t *msgs = NULL;
    size_t msgs_count = 0;
    err = sc_context_format_messages(agent->alloc, agent->history, agent->history_count,
                                     agent->max_history_messages, &msgs, &msgs_count);
    if (err != SC_OK) {
        if (system_prompt)
            agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
        return err;
    }

    size_t total_msgs = (msgs ? msgs_count : 0) + 1;
    sc_chat_message_t *all_msgs = (sc_chat_message_t *)agent->alloc->alloc(
        agent->alloc->ctx, total_msgs * sizeof(sc_chat_message_t));
    if (!all_msgs) {
        if (system_prompt)
            agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
        if (msgs)
            agent->alloc->free(agent->alloc->ctx, msgs, msgs_count * sizeof(sc_chat_message_t));
        return SC_ERR_OUT_OF_MEMORY;
    }
    all_msgs[0].role = SC_ROLE_SYSTEM;
    all_msgs[0].content = system_prompt;
    all_msgs[0].content_len = system_prompt_len;
    all_msgs[0].name = NULL;
    all_msgs[0].name_len = 0;
    all_msgs[0].tool_call_id = NULL;
    all_msgs[0].tool_call_id_len = 0;
    all_msgs[0].content_parts = NULL;
    all_msgs[0].content_parts_count = 0;
    for (size_t i = 0; i < (msgs ? msgs_count : 0); i++)
        all_msgs[i + 1] = msgs[i];
    if (msgs)
        agent->alloc->free(agent->alloc->ctx, msgs, msgs_count * sizeof(sc_chat_message_t));
    msgs = all_msgs;
    msgs_count = total_msgs;

    sc_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.messages_count = msgs_count;
    req.model = agent->model_name;
    req.model_len = agent->model_name_len;
    req.temperature = agent->temperature;
    req.tools = (agent->tool_specs_count > 0) ? agent->tool_specs : NULL;
    req.tools_count = agent->tool_specs_count;

    {
        stream_token_wrap_t wrap = {.on_token = on_token, .token_ctx = token_ctx};
        sc_stream_chat_result_t sresp;
        memset(&sresp, 0, sizeof(sresp));
        err = agent->provider.vtable->stream_chat(
            agent->provider.ctx, agent->alloc, &req, agent->model_name, agent->model_name_len,
            agent->temperature, stream_chunk_to_token_cb, &wrap, &sresp);
        if (msgs)
            agent->alloc->free(agent->alloc->ctx, msgs, msgs_count * sizeof(sc_chat_message_t));
        if (system_prompt)
            agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
        if (err != SC_OK) {
            sc_agent_clear_current_for_tools();
            return err;
        }
        agent->total_tokens += sresp.usage.total_tokens;
        agent_record_cost(agent, &sresp.usage);
        if (sresp.content && sresp.content_len > 0) {
            (void)append_history(agent, SC_ROLE_ASSISTANT, sresp.content, sresp.content_len, NULL,
                                 0, NULL, 0);
            *response_out = sc_strndup(agent->alloc, sresp.content, sresp.content_len);
            if (!*response_out)
                return SC_ERR_OUT_OF_MEMORY;
            if (response_len_out)
                *response_len_out = sresp.content_len;
            agent_maybe_tts(agent, sresp.content, sresp.content_len);
        }
        sc_agent_clear_current_for_tools();
        return SC_OK;
    }
}

/* ── Planner execution (Tier 1.4) ────────────────────────────────────── */

static sc_error_t execute_plan_steps(sc_agent_t *agent, sc_plan_t *plan, char **summary_out,
                                     size_t *summary_len_out) {
    char result_buf[4096];
    int result_off = 0;
    result_off += snprintf(result_buf + result_off, sizeof(result_buf) - (size_t)result_off,
                           "Plan: %zu steps\n", plan->steps_count);

    for (size_t i = 0; i < plan->steps_count; i++) {
        if (agent->cancel_requested) {
            sc_planner_mark_step(plan, i, SC_PLAN_STEP_FAILED);
            result_off += snprintf(result_buf + result_off, sizeof(result_buf) - (size_t)result_off,
                                   "  [%zu] %s: CANCELLED\n", i + 1, plan->steps[i].tool_name);
            continue;
        }

        sc_planner_mark_step(plan, i, SC_PLAN_STEP_RUNNING);

        {
            sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}}};
            ev.data.tool_call_start.tool = plan->steps[i].tool_name;
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        sc_tool_t *tool =
            find_tool(agent, plan->steps[i].tool_name, strlen(plan->steps[i].tool_name));
        if (!tool) {
            sc_planner_mark_step(plan, i, SC_PLAN_STEP_FAILED);
            result_off += snprintf(result_buf + result_off, sizeof(result_buf) - (size_t)result_off,
                                   "  [%zu] %s: tool not found\n", i + 1, plan->steps[i].tool_name);
            {
                sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_CALL, .data = {{0}}};
                ev.data.tool_call.tool = plan->steps[i].tool_name;
                ev.data.tool_call.success = false;
                ev.data.tool_call.detail = "tool not found";
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            continue;
        }

        sc_json_value_t *args = NULL;
        if (plan->steps[i].args_json) {
            size_t args_len = strlen(plan->steps[i].args_json);
            sc_error_t pe = sc_json_parse(agent->alloc, plan->steps[i].args_json, args_len, &args);
            if (pe != SC_OK)
                args = NULL;
        }

        sc_tool_result_t result = sc_tool_result_fail("invalid arguments", 16);
        clock_t tool_start = clock();
        if (args) {
            tool->vtable->execute(tool->ctx, agent->alloc, args, &result);
            sc_json_free(agent->alloc, args);
        }
        uint64_t tool_duration_ms = clock_diff_ms(tool_start, clock());

        bool ok = result.success;
        sc_planner_mark_step(plan, i, ok ? SC_PLAN_STEP_DONE : SC_PLAN_STEP_FAILED);

        {
            sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_CALL, .data = {{0}}};
            ev.data.tool_call.tool = plan->steps[i].tool_name;
            ev.data.tool_call.duration_ms = tool_duration_ms;
            ev.data.tool_call.success = ok;
            ev.data.tool_call.detail = ok ? NULL : (result.error_msg ? result.error_msg : "failed");
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        const char *desc =
            plan->steps[i].description ? plan->steps[i].description : plan->steps[i].tool_name;
        result_off += snprintf(result_buf + result_off, sizeof(result_buf) - (size_t)result_off,
                               "  [%zu] %s: %s (%llums)\n", i + 1, desc, ok ? "done" : "FAILED",
                               (unsigned long long)tool_duration_ms);

        sc_tool_result_free(agent->alloc, &result);
    }

    *summary_out = sc_strndup(agent->alloc, result_buf, (size_t)result_off);
    if (!*summary_out)
        return SC_ERR_OUT_OF_MEMORY;
    if (summary_len_out)
        *summary_len_out = (size_t)result_off;
    return SC_OK;
}

sc_error_t sc_agent_execute_plan(sc_agent_t *agent, const char *plan_json, size_t plan_json_len,
                                 char **summary_out, size_t *summary_len_out) {
    if (!agent || !plan_json || !summary_out)
        return SC_ERR_INVALID_ARGUMENT;
    *summary_out = NULL;
    if (summary_len_out)
        *summary_len_out = 0;

    sc_plan_t *plan = NULL;
    sc_error_t err = sc_planner_create_plan(agent->alloc, plan_json, plan_json_len, &plan);
    if (err != SC_OK)
        return err;

    err = execute_plan_steps(agent, plan, summary_out, summary_len_out);
    sc_plan_free(agent->alloc, plan);
    return err;
}
