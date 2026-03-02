#include "seaclaw/agent.h"
#include <stdint.h>
#include "seaclaw/agent/dispatcher.h"
#include "seaclaw/agent/compaction.h"
#include "seaclaw/agent/planner.h"
#include "seaclaw/agent/prompt.h"
#include "seaclaw/agent/memory_loader.h"
#include "seaclaw/context.h"
#include "seaclaw/core/string.h"
#include "seaclaw/core/json.h"
#include "seaclaw/observer.h"
#include "seaclaw/provider.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#define SC_OBS_SAFE_RECORD_EVENT(agent, ev) \
    do { if ((agent)->observer) sc_observer_record_event(*(agent)->observer, (ev)); } while (0)

static uint64_t clock_diff_ms(clock_t start, clock_t end) {
    return (uint64_t)((end - start) * 1000 / CLOCKS_PER_SEC);
}

#define SC_AGENT_HISTORY_INIT_CAP 16
#define SC_AGENT_MAX_SLASH_LEN 256

static bool is_slash_command(const char *msg, size_t len) {
    if (!msg || len == 0) return false;
    size_t i = 0;
    while (i < len && (msg[i] == ' ' || msg[i] == '\t')) i++;
    if (i >= len) return false;
    return msg[i] == '/';
}

static void parse_slash(const char *msg, size_t len,
    char *cmd_buf, size_t cmd_cap, size_t *cmd_len,
    char *arg_buf, size_t arg_cap, size_t *arg_len)
{
    *cmd_len = 0;
    *arg_len = 0;
    if (cmd_cap) cmd_buf[0] = '\0';
    if (arg_cap) arg_buf[0] = '\0';
    size_t i = 0;
    while (i < len && (msg[i] == ' ' || msg[i] == '\t')) i++;
    if (i >= len || msg[i] != '/') return;
    i++;
    size_t cmd_start = i;
    while (i < len && msg[i] != ' ' && msg[i] != '\t' && msg[i] != ':') i++;
    size_t cmd_end = i;
    if (cmd_end > cmd_start && cmd_end - cmd_start < cmd_cap) {
        memcpy(cmd_buf, msg + cmd_start, cmd_end - cmd_start);
        cmd_buf[cmd_end - cmd_start] = '\0';
        *cmd_len = cmd_end - cmd_start;
    }
    while (i < len && (msg[i] == ' ' || msg[i] == '\t' || msg[i] == ':')) i++;
    size_t arg_start = i;
    while (i < len && msg[i] != '\n' && msg[i] != '\r') i++;
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
        if (ca != cb) return (int)(unsigned char)ca - (unsigned char)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

sc_error_t sc_agent_from_config(sc_agent_t *out, sc_allocator_t *alloc,
    sc_provider_t provider,
    const sc_tool_t *tools, size_t tools_count,
    sc_memory_t *memory,
    sc_session_store_t *session_store,
    sc_observer_t *observer,
    sc_security_policy_t *policy,
    const char *model_name, size_t model_name_len,
    const char *default_provider, size_t default_provider_len,
    double temperature,
    const char *workspace_dir, size_t workspace_dir_len,
    uint32_t max_tool_iterations, uint32_t max_history_messages,
    bool auto_save,
    uint8_t autonomy_level,
    const char *custom_instructions, size_t custom_instructions_len)
{
    if (!out || !alloc || !provider.vtable || !model_name) {
        return SC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));

    out->alloc = alloc;
    out->provider = provider;
    out->memory = memory;
    out->session_store = session_store;
    out->observer = observer;
    out->policy = policy;
    out->temperature = temperature;
    out->max_tool_iterations = max_tool_iterations ? max_tool_iterations : 25;
    out->max_history_messages = max_history_messages ? max_history_messages : 50;
    out->auto_save = auto_save;
    out->autonomy_level = autonomy_level;
    out->custom_instructions = NULL;
    out->custom_instructions_len = 0;
    if (custom_instructions && custom_instructions_len > 0) {
        out->custom_instructions = sc_strndup(alloc, custom_instructions, custom_instructions_len);
        if (!out->custom_instructions) {
            alloc->free(alloc->ctx, out->workspace_dir, out->workspace_dir_len + 1);
            alloc->free(alloc->ctx, out->default_provider, out->default_provider_len + 1);
            alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
            return SC_ERR_OUT_OF_MEMORY;
        }
        out->custom_instructions_len = custom_instructions_len;
    }

    out->model_name = sc_strndup(alloc, model_name, model_name_len);
    if (!out->model_name) return SC_ERR_OUT_OF_MEMORY;
    out->model_name_len = model_name_len;

    out->default_provider = sc_strndup(alloc,
        default_provider ? default_provider : "openai",
        default_provider_len ? default_provider_len : strlen("openai"));
    if (!out->default_provider) {
        alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    out->default_provider_len = default_provider_len ? default_provider_len : strlen("openai");

    out->workspace_dir = sc_strndup(alloc,
        workspace_dir ? workspace_dir : ".",
        workspace_dir_len ? workspace_dir_len : 1);
    if (!out->workspace_dir) {
        alloc->free(alloc->ctx, out->default_provider, out->default_provider_len + 1);
        alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    out->workspace_dir_len = workspace_dir_len ? workspace_dir_len : 1;

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

        out->tool_specs = (sc_tool_spec_t *)alloc->alloc(alloc->ctx,
            tools_count * sizeof(sc_tool_spec_t));
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
            out->tool_specs[i].name_len = out->tool_specs[i].name ? strlen(out->tool_specs[i].name) : 0;
            out->tool_specs[i].description = out->tools[i].vtable->description(out->tools[i].ctx);
            out->tool_specs[i].description_len = out->tool_specs[i].description ? strlen(out->tool_specs[i].description) : 0;
            out->tool_specs[i].parameters_json = out->tools[i].vtable->parameters_json(out->tools[i].ctx);
            out->tool_specs[i].parameters_json_len = out->tool_specs[i].parameters_json ? strlen(out->tool_specs[i].parameters_json) : 0;
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
        sc_error_t perr = sc_prompt_build_static(alloc, &cfg,
            &out->cached_static_prompt, &out->cached_static_prompt_len);
        if (perr != SC_OK) {
            out->cached_static_prompt = NULL;
            out->cached_static_prompt_len = 0;
        }
        out->cached_static_prompt_cap = out->cached_static_prompt_len;
    }

    out->turn_arena = sc_arena_create(*alloc);

    return SC_OK;
}

void sc_agent_deinit(sc_agent_t *agent) {
    if (!agent) return;
    sc_agent_clear_history(agent);
    if (agent->history) {
        agent->alloc->free(agent->alloc->ctx, agent->history,
            agent->history_cap * sizeof(sc_chat_message_t));
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
        agent->alloc->free(agent->alloc->ctx, agent->default_provider, agent->default_provider_len + 1);
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
    if (agent->provider.vtable && agent->provider.vtable->deinit) {
        agent->provider.vtable->deinit(agent->provider.ctx, agent->alloc);
    }
    if (agent->turn_arena) {
        sc_arena_destroy(agent->turn_arena);
        agent->turn_arena = NULL;
    }
}

static sc_error_t ensure_history_cap(sc_agent_t *agent, size_t need) {
    if (agent->history_cap >= need) return SC_OK;
    size_t new_cap = agent->history_cap ? agent->history_cap : SC_AGENT_HISTORY_INIT_CAP;
    while (new_cap < need) new_cap *= 2;
    sc_owned_message_t *new_arr = (sc_owned_message_t *)agent->alloc->realloc(
        agent->alloc->ctx, agent->history,
        agent->history_cap * sizeof(sc_owned_message_t),
        new_cap * sizeof(sc_owned_message_t));
    if (!new_arr) return SC_ERR_OUT_OF_MEMORY;
    agent->history = new_arr;
    agent->history_cap = new_cap;
    return SC_OK;
}

static sc_error_t append_history(sc_agent_t *agent, sc_role_t role,
    const char *content, size_t content_len,
    const char *name, size_t name_len,
    const char *tool_call_id, size_t tool_call_id_len)
{
    sc_error_t err = ensure_history_cap(agent, agent->history_count + 1);
    if (err != SC_OK) return err;
    char *dup = sc_strndup(agent->alloc, content, content_len);
    if (!dup) return SC_ERR_OUT_OF_MEMORY;
    agent->history[agent->history_count].role = role;
    agent->history[agent->history_count].content = dup;
    agent->history[agent->history_count].content_len = content_len;
    agent->history[agent->history_count].name = name_len ? sc_strndup(agent->alloc, name, name_len) : NULL;
    agent->history[agent->history_count].name_len = name_len;
    agent->history[agent->history_count].tool_call_id = tool_call_id_len ? sc_strndup(agent->alloc, tool_call_id, tool_call_id_len) : NULL;
    agent->history[agent->history_count].tool_call_id_len = tool_call_id_len;
    agent->history[agent->history_count].tool_calls = NULL;
    agent->history[agent->history_count].tool_calls_count = 0;
    agent->history_count++;
    return SC_OK;
}

/* Append assistant message with tool_calls (duplicates and owns tool_calls). */
static sc_error_t append_history_with_tool_calls(sc_agent_t *agent,
    const char *content, size_t content_len,
    const sc_tool_call_t *tool_calls, size_t tool_calls_count)
{
    sc_error_t err = ensure_history_cap(agent, agent->history_count + 1);
    if (err != SC_OK) return err;
    char *dup = content && content_len > 0
        ? sc_strndup(agent->alloc, content, content_len)
        : sc_strndup(agent->alloc, "", 0);
    if (!dup) return SC_ERR_OUT_OF_MEMORY;
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
        sc_tool_call_t *owned = (sc_tool_call_t *)agent->alloc->alloc(agent->alloc->ctx,
            tool_calls_count * sizeof(sc_tool_call_t));
        if (!owned) {
            agent->alloc->free(agent->alloc->ctx, dup, content_len + 1);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memset(owned, 0, tool_calls_count * sizeof(sc_tool_call_t));
        for (size_t i = 0; i < tool_calls_count; i++) {
            const sc_tool_call_t *src = &tool_calls[i];
            if (src->id && src->id_len > 0) {
                owned[i].id = sc_strndup(agent->alloc, src->id, src->id_len);
                owned[i].id_len = src->id_len;
            }
            if (src->name && src->name_len > 0) {
                owned[i].name = sc_strndup(agent->alloc, src->name, src->name_len);
                owned[i].name_len = src->name_len;
            }
            if (src->arguments && src->arguments_len > 0) {
                owned[i].arguments = sc_strndup(agent->alloc, src->arguments, src->arguments_len);
                owned[i].arguments_len = src->arguments_len;
            }
        }
        agent->history[agent->history_count].tool_calls = owned;
        agent->history[agent->history_count].tool_calls_count = tool_calls_count;
    }
    agent->history_count++;
    return SC_OK;
}

static void free_owned_tool_calls(sc_allocator_t *alloc,
    sc_tool_call_t *tcs, size_t count)
{
    if (!tcs || count == 0) return;
    for (size_t i = 0; i < count; i++) {
        if (tcs[i].id) alloc->free(alloc->ctx, (void *)tcs[i].id, tcs[i].id_len + 1);
        if (tcs[i].name) alloc->free(alloc->ctx, (void *)tcs[i].name, tcs[i].name_len + 1);
        if (tcs[i].arguments) alloc->free(alloc->ctx, (void *)tcs[i].arguments,
            tcs[i].arguments_len + 1);
    }
    alloc->free(alloc->ctx, tcs, count * sizeof(sc_tool_call_t));
}

void sc_agent_clear_history(sc_agent_t *agent) {
    if (!agent) return;
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
        (void)agent->session_store->vtable->clear_messages(agent->session_store->ctx,
            "", 0);
    }
}

uint32_t sc_agent_estimate_tokens(const char *text, size_t len) {
    if (!text) return 0;
    return (uint32_t)((len + 3) / 4);
}

char *sc_agent_handle_slash_command(sc_agent_t *agent,
    const char *message, size_t message_len)
{
    if (!agent || !message || !is_slash_command(message, message_len)) {
        return NULL;
    }
    char cmd_buf[64], arg_buf[192];
    size_t cmd_len, arg_len;
    parse_slash(message, message_len, cmd_buf, sizeof(cmd_buf), &cmd_len,
        arg_buf, sizeof(arg_buf), &arg_len);

    if (cmd_len == 0) return NULL;

    if (sc_strncasecmp(cmd_buf, "help", 4) == 0 ||
        sc_strncasecmp(cmd_buf, "commands", 8) == 0) {
        const char *help = "Commands:\n"
            "  /help, /commands   Show this help\n"
            "  /quit, /exit      End session\n"
            "  /clear            Clear conversation history\n"
            "  /model [name]     Show or switch model\n"
            "  /provider         Show current provider\n"
            "  /tools            List available tools\n"
            "  /plan <json>      Execute a structured plan\n"
            "  /cost             Show token usage\n"
            "  /status           Show agent status\n";
        return sc_strndup(agent->alloc, help, strlen(help));
    }

    if (sc_strncasecmp(cmd_buf, "quit", 4) == 0 ||
        sc_strncasecmp(cmd_buf, "exit", 4) == 0) {
        return sc_strndup(agent->alloc, "Goodbye.", 8);
    }

    if (sc_strncasecmp(cmd_buf, "clear", 5) == 0 ||
        sc_strncasecmp(cmd_buf, "new", 3) == 0 ||
        sc_strncasecmp(cmd_buf, "reset", 5) == 0) {
        sc_agent_clear_history(agent);
        return sc_strndup(agent->alloc, "History cleared.", 16);
    }

    if (sc_strncasecmp(cmd_buf, "model", 5) == 0) {
        if (arg_len > 0) {
            char *old = agent->model_name;
            size_t old_len = agent->model_name_len;
            agent->model_name = sc_strndup(agent->alloc, arg_buf, arg_len);
            if (!agent->model_name) return NULL;
            agent->model_name_len = arg_len;
            if (old) agent->alloc->free(agent->alloc->ctx, old, old_len + 1);

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
            (void)sc_prompt_build_static(agent->alloc, &pcfg,
                &agent->cached_static_prompt, &agent->cached_static_prompt_len);
            agent->cached_static_prompt_cap = agent->cached_static_prompt_len;
        }
        return sc_sprintf(agent->alloc, "Model: %.*s",
            (int)agent->model_name_len, agent->model_name);
    }

    if (sc_strncasecmp(cmd_buf, "status", 6) == 0) {
        const char *prov = agent->provider.vtable->get_name(agent->provider.ctx);
        return sc_sprintf(agent->alloc,
            "Provider: %s | Model: %.*s | History: %zu messages | Tokens: %llu",
            prov ? prov : "?",
            (int)agent->model_name_len, agent->model_name,
            (size_t)agent->history_count,
            (unsigned long long)agent->total_tokens);
    }

    if (sc_strncasecmp(cmd_buf, "cost", 4) == 0) {
        return sc_sprintf(agent->alloc,
            "Tokens used: %llu (est. cost depends on provider pricing)\n"
            "History: %zu messages",
            (unsigned long long)agent->total_tokens,
            (size_t)agent->history_count);
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
        if (!buf) return NULL;
        int off = snprintf(buf, 4096, "Tools (%zu):\n", agent->tools_count);
        for (size_t i = 0; i < agent->tools_count && off < 4000; i++) {
            const char *n = agent->tools[i].vtable->name(agent->tools[i].ctx);
            off += snprintf(buf + off, 4096 - (size_t)off, "  %s\n", n ? n : "?");
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

    return NULL;
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

sc_error_t sc_agent_run_single(sc_agent_t *agent,
    const char *system_prompt, size_t system_prompt_len,
    const char *user_message, size_t user_message_len,
    char **response_out, size_t *response_len_out)
{
    if (!agent || !response_out) return SC_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out) *response_len_out = 0;

    sc_chat_request_t req;
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
    sc_error_t err = agent->provider.vtable->chat(agent->provider.ctx, agent->alloc,
        &req, agent->model_name, agent->model_name_len, agent->temperature, &resp);
    if (err != SC_OK) return err;

    if (resp.content && resp.content_len > 0) {
        *response_out = sc_strndup(agent->alloc, resp.content, resp.content_len);
        if (!*response_out) {
            sc_chat_response_free(agent->alloc, &resp);
            return SC_ERR_OUT_OF_MEMORY;
        }
        if (response_len_out) *response_len_out = resp.content_len;
    }
    agent->total_tokens += resp.usage.total_tokens;
    sc_chat_response_free(agent->alloc, &resp);
    return SC_OK;
}

sc_error_t sc_agent_turn(sc_agent_t *agent, const char *msg, size_t msg_len,
    char **response_out, size_t *response_len_out)
{
    if (!agent || !msg || !response_out) return SC_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out) *response_len_out = 0;

    char *slash_resp = sc_agent_handle_slash_command(agent, msg, msg_len);
    if (slash_resp) {
        *response_out = slash_resp;
        if (response_len_out) *response_len_out = strlen(slash_resp);
        return SC_OK;
    }

    sc_error_t err = append_history(agent, SC_ROLE_USER, msg, msg_len, NULL, 0, NULL, 0);
    if (err != SC_OK) return err;

    /* Load memory context for this turn */
    char *memory_ctx = NULL;
    size_t memory_ctx_len = 0;
    if (agent->memory && agent->memory->vtable) {
        sc_memory_loader_t loader;
        sc_memory_loader_init(&loader, agent->alloc, agent->memory, 10, 4000);
        (void)sc_memory_loader_load(&loader, msg, msg_len, "", 0, &memory_ctx, &memory_ctx_len);
    }

    /* Build system prompt using cached static portion when available */
    char *system_prompt = NULL;
    size_t system_prompt_len = 0;
    if (agent->cached_static_prompt) {
        err = sc_prompt_build_with_cache(agent->alloc,
            agent->cached_static_prompt, agent->cached_static_prompt_len,
            memory_ctx, memory_ctx_len,
            &system_prompt, &system_prompt_len);
        if (memory_ctx) agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (err != SC_OK) return err;
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
        };
        err = sc_prompt_build_system(agent->alloc, &cfg, &system_prompt, &system_prompt_len);
        if (memory_ctx) agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (err != SC_OK) return err;
    }

    sc_chat_message_t *msgs = NULL;
    size_t msgs_count = 0;

    sc_chat_request_t req;
    req.messages = msgs;
    req.messages_count = msgs_count;
    req.model = agent->model_name;
    req.model_len = agent->model_name_len;
    req.temperature = agent->temperature;
    req.max_tokens = 0;
    req.tools = (agent->tool_specs_count > 0) ? agent->tool_specs : NULL;
    req.tools_count = agent->tool_specs_count;
    req.timeout_secs = 0;
    req.reasoning_effort = NULL;
    req.reasoning_effort_len = 0;

    uint32_t iter = 0;
    sc_compaction_config_t compact_cfg;
    sc_compaction_config_default(&compact_cfg);
    compact_cfg.max_history_messages = agent->max_history_messages;
    compact_cfg.token_limit = 32000;

    clock_t turn_start = clock();
    uint64_t turn_tokens = 0;
    const char *prov_name = agent->provider.vtable->get_name ? agent->provider.vtable->get_name(agent->provider.ctx) : NULL;

    {
        sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_AGENT_START, .data = {{0}} };
        ev.data.agent_start.provider = prov_name ? prov_name : "";
        ev.data.agent_start.model = agent->model_name ? agent->model_name : "";
        SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }

    /* Per-turn arena: reset each iteration to reclaim ephemeral message arrays */
    sc_allocator_t turn_alloc = agent->turn_arena ?
        sc_arena_allocator(agent->turn_arena) : *agent->alloc;

    while (iter < agent->max_tool_iterations) {
        if (agent->cancel_requested) {
            if (system_prompt) agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena) sc_arena_reset(agent->turn_arena);
            return SC_ERR_CANCELLED;
        }
        if (agent->turn_arena) sc_arena_reset(agent->turn_arena);
        msgs = NULL;
        msgs_count = 0;
        iter++;
        /* Compact history if it exceeds limits (before each provider call) */
        if (sc_should_compact(agent->history, agent->history_count, &compact_cfg)) {
            sc_compact_history(agent->alloc, agent->history, &agent->history_count,
                &agent->history_cap, &compact_cfg);
        }

        /* Format messages for this iteration using arena allocator */
        {
            sc_chat_message_t *hist_msgs = NULL;
            size_t hist_count = 0;
            err = sc_context_format_messages(&turn_alloc, agent->history, agent->history_count,
                agent->max_history_messages, &hist_msgs, &hist_count);
            if (err != SC_OK) {
                if (system_prompt) agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                return err;
            }
            size_t total = (hist_msgs ? hist_count : 0) + 1;
            sc_chat_message_t *all = (sc_chat_message_t *)turn_alloc.alloc(turn_alloc.ctx,
                total * sizeof(sc_chat_message_t));
            if (!all) {
                if (system_prompt) agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
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
            sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_LLM_REQUEST, .data = {{0}} };
            ev.data.llm_request.provider = prov_name ? prov_name : "";
            ev.data.llm_request.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_request.messages_count = msgs_count;
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        clock_t llm_start = clock();
        sc_chat_response_t resp;
        memset(&resp, 0, sizeof(resp));
        err = agent->provider.vtable->chat(agent->provider.ctx, agent->alloc,
            &req, agent->model_name, agent->model_name_len, agent->temperature, &resp);
        uint64_t llm_duration_ms = clock_diff_ms(llm_start, clock());

        {
            sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}} };
            ev.data.llm_response.provider = prov_name ? prov_name : "";
            ev.data.llm_response.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_response.duration_ms = llm_duration_ms;
            ev.data.llm_response.success = (err == SC_OK);
            ev.data.llm_response.error_message = (err != SC_OK) ? "chat failed" : NULL;
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        if (err != SC_OK) {
            {
                sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_ERR, .data = {{0}} };
                ev.data.err.component = "agent";
                ev.data.err.message = "provider chat failed";
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            if (system_prompt) agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena) sc_arena_reset(agent->turn_arena);
            return err;
        }

        agent->total_tokens += resp.usage.total_tokens;
        turn_tokens += resp.usage.total_tokens;

        if (resp.tool_calls_count == 0) {
            uint64_t turn_duration_ms = clock_diff_ms(turn_start, clock());
            {
                sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_AGENT_END, .data = {{0}} };
                ev.data.agent_end.duration_ms = turn_duration_ms;
                ev.data.agent_end.tokens_used = turn_tokens;
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            {
                sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TURN_COMPLETE, .data = {{0}} };
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            if (resp.content && resp.content_len > 0) {
                (void)append_history(agent, SC_ROLE_ASSISTANT, resp.content, resp.content_len, NULL, 0, NULL, 0);
                *response_out = sc_strndup(agent->alloc, resp.content, resp.content_len);
                if (!*response_out) {
                    sc_chat_response_free(agent->alloc, &resp);
                    if (system_prompt) agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                    if (agent->turn_arena) sc_arena_reset(agent->turn_arena);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                if (response_len_out) *response_len_out = resp.content_len;
            }
            sc_chat_response_free(agent->alloc, &resp);
            if (system_prompt) agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena) sc_arena_reset(agent->turn_arena);
            return SC_OK;
        }

        err = append_history_with_tool_calls(agent,
            resp.content ? resp.content : "", resp.content_len,
            resp.tool_calls, resp.tool_calls_count);
        if (err != SC_OK) {
            sc_chat_response_free(agent->alloc, &resp);
            if (system_prompt) agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena) sc_arena_reset(agent->turn_arena);
            return err;
        }
        sc_chat_response_free(agent->alloc, &resp);

        {
            size_t tc_count = agent->history[agent->history_count - 1].tool_calls_count;
            const sc_tool_call_t *calls = agent->history[agent->history_count - 1].tool_calls;

            /* Emit TOOL_CALL_START events for all calls */
            for (size_t tc = 0; tc < tc_count; tc++) {
                char tn_buf[64];
                size_t tn = (calls[tc].name_len < sizeof(tn_buf) - 1) ? calls[tc].name_len : sizeof(tn_buf) - 1;
                if (tn > 0 && calls[tc].name) memcpy(tn_buf, calls[tc].name, tn);
                tn_buf[tn] = '\0';
                sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}} };
                ev.data.tool_call_start.tool = tn_buf[0] ? tn_buf : "unknown";
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }

            /* Use dispatcher for parallel execution when enabled (Tier 1.3) */
            sc_dispatcher_t dispatcher;
            sc_dispatcher_default(&dispatcher);
            if (tc_count > 1) dispatcher.max_parallel = 4;
            dispatcher.timeout_secs = 30;

            sc_dispatch_result_t dispatch_result;
            memset(&dispatch_result, 0, sizeof(dispatch_result));
            err = sc_dispatcher_dispatch(&dispatcher, agent->alloc,
                agent->tools, agent->tools_count,
                calls, tc_count, &dispatch_result);

            if (err == SC_OK && dispatch_result.results) {
                for (size_t tc = 0; tc < tc_count; tc++) {
                    const sc_tool_call_t *call = &calls[tc];
                    sc_tool_result_t *result = &dispatch_result.results[tc];

                    /* Approval flow: if tool needs approval, ask user and retry */
                    if (result->needs_approval && agent->approval_cb) {
                        char tn_tmp[64];
                        size_t tn2 = (call->name_len < sizeof(tn_tmp) - 1) ? call->name_len : sizeof(tn_tmp) - 1;
                        if (tn2 > 0 && call->name) memcpy(tn_tmp, call->name, tn2);
                        tn_tmp[tn2] = '\0';
                        const char *args_str = call->arguments ? call->arguments : "";
                        bool user_approved = agent->approval_cb(agent->approval_ctx, tn_tmp, args_str);
                        if (user_approved) {
                            sc_tool_result_free(agent->alloc, result);
                            if (agent->policy) agent->policy->pre_approved = true;
                            sc_tool_t *tool = find_tool(agent, call->name, call->name_len);
                            if (tool) {
                                sc_json_value_t *retry_args = NULL;
                                if (call->arguments_len > 0)
                                    (void)sc_json_parse(agent->alloc, call->arguments, call->arguments_len, &retry_args);
                                *result = sc_tool_result_fail("invalid arguments", 16);
                                if (retry_args) {
                                    if (tool->vtable->execute)
                                        tool->vtable->execute(tool->ctx, agent->alloc, retry_args, result);
                                    sc_json_free(agent->alloc, retry_args);
                                }
                            }
                        }
                    }

                    char tn_buf[64];
                    size_t tn = (call->name_len < sizeof(tn_buf) - 1) ? call->name_len : sizeof(tn_buf) - 1;
                    if (tn > 0 && call->name) memcpy(tn_buf, call->name, tn);
                    tn_buf[tn] = '\0';

                    {
                        sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TOOL_CALL, .data = {{0}} };
                        ev.data.tool_call.tool = tn_buf[0] ? tn_buf : "unknown";
                        ev.data.tool_call.duration_ms = 0;
                        ev.data.tool_call.success = result->success;
                        ev.data.tool_call.detail = result->success ? NULL :
                            (result->error_msg ? result->error_msg : "failed");
                        SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
                    }

                    const char *res_content = result->success ? result->output : result->error_msg;
                    size_t res_len = result->success ? result->output_len : result->error_msg_len;
                    (void)append_history(agent, SC_ROLE_TOOL, res_content, res_len,
                        call->name, call->name_len, call->id, call->id_len);

                    if (agent->cancel_requested) break;
                }
                sc_dispatch_result_free(agent->alloc, &dispatch_result);
            } else {
                /* Fallback: sequential if dispatcher fails */
                for (size_t tc = 0; tc < tc_count; tc++) {
                    const sc_tool_call_t *call = &calls[tc];
                    sc_tool_t *tool = find_tool(agent, call->name, call->name_len);
                    if (!tool) {
                        (void)append_history(agent, SC_ROLE_TOOL, "tool not found", 14,
                            call->name, call->name_len, call->id, call->id_len);
                        continue;
                    }
                    sc_json_value_t *args = NULL;
                    if (call->arguments_len > 0) {
                        sc_error_t pe = sc_json_parse(agent->alloc, call->arguments, call->arguments_len, &args);
                        if (pe != SC_OK) args = NULL;
                    }
                    sc_tool_result_t result = sc_tool_result_fail("invalid arguments", 16);
                    if (args) {
                        tool->vtable->execute(tool->ctx, agent->alloc, args, &result);
                        sc_json_free(agent->alloc, args);
                    }

                    /* Approval retry for sequential fallback path */
                    if (result.needs_approval && agent->approval_cb) {
                        char seq_tn[64];
                        size_t seq_n = (call->name_len < sizeof(seq_tn) - 1) ? call->name_len : sizeof(seq_tn) - 1;
                        if (seq_n > 0 && call->name) memcpy(seq_tn, call->name, seq_n);
                        seq_tn[seq_n] = '\0';
                        if (agent->approval_cb(agent->approval_ctx, seq_tn,
                                call->arguments ? call->arguments : "")) {
                            sc_tool_result_free(agent->alloc, &result);
                            if (agent->policy) agent->policy->pre_approved = true;
                            sc_json_value_t *retry_args = NULL;
                            if (call->arguments_len > 0)
                                (void)sc_json_parse(agent->alloc, call->arguments, call->arguments_len, &retry_args);
                            result = sc_tool_result_fail("invalid arguments", 16);
                            if (retry_args) {
                                tool->vtable->execute(tool->ctx, agent->alloc, retry_args, &result);
                                sc_json_free(agent->alloc, retry_args);
                            }
                        }
                    }

                    const char *res_content = result.success ? result.output : result.error_msg;
                    size_t res_len = result.success ? result.output_len : result.error_msg_len;
                    (void)append_history(agent, SC_ROLE_TOOL, res_content, res_len,
                        call->name, call->name_len, call->id, call->id_len);
                    sc_tool_result_free(agent->alloc, &result);
                    if (agent->cancel_requested) break;
                }
            }
        }
        /* Messages will be reformatted at top of next iteration via arena */
    }

    {
        sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED, .data = {{0}} };
        ev.data.tool_iterations_exhausted.iterations = agent->max_tool_iterations;
        SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }
    {
        sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_ERR, .data = {{0}} };
        ev.data.err.component = "agent";
        ev.data.err.message = "tool iterations exhausted";
        SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }
    if (system_prompt) agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
    if (agent->turn_arena) sc_arena_reset(agent->turn_arena);
    return SC_ERR_TIMEOUT;
}

typedef struct stream_token_wrap {
    sc_agent_stream_token_cb on_token;
    void *token_ctx;
} stream_token_wrap_t;

static void stream_chunk_to_token_cb(void *ctx, const sc_stream_chunk_t *chunk) {
    stream_token_wrap_t *w = (stream_token_wrap_t *)ctx;
    if (chunk->is_final || !w->on_token) return;
    if (chunk->delta && chunk->delta_len > 0)
        w->on_token(chunk->delta, chunk->delta_len, w->token_ctx);
}

sc_error_t sc_agent_turn_stream(sc_agent_t *agent, const char *msg, size_t msg_len,
    sc_agent_stream_token_cb on_token, void *token_ctx,
    char **response_out, size_t *response_len_out)
{
    if (!agent || !msg || !response_out) return SC_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out) *response_len_out = 0;

    char *slash_resp = sc_agent_handle_slash_command(agent, msg, msg_len);
    if (slash_resp) {
        *response_out = slash_resp;
        if (response_len_out) *response_len_out = strlen(slash_resp);
        return SC_OK;
    }

    bool can_stream = (on_token != NULL) &&
        agent->provider.vtable->supports_streaming &&
        agent->provider.vtable->supports_streaming(agent->provider.ctx) &&
        agent->provider.vtable->stream_chat;

    if (!can_stream) {
        sc_error_t fallback_err = sc_agent_turn(agent, msg, msg_len, response_out, response_len_out);
        if (fallback_err == SC_OK && on_token && *response_out && response_len_out && *response_len_out > 0) {
            size_t chunk_size = 12;
            for (size_t i = 0; i < *response_len_out; i += chunk_size) {
                size_t n = *response_len_out - i;
                if (n > chunk_size) n = chunk_size;
                on_token(*response_out + i, n, token_ctx);
            }
        }
        return fallback_err;
    }

    /* When tools are present, fall back to sc_agent_turn but simulate
     * streaming by chunking the final response to the callback.
     * This keeps the TUI responsive while tools execute. (Tier 2.4) */
    bool has_tools = (agent->tool_specs_count > 0);
    if (has_tools) {
        sc_error_t fallback_err = sc_agent_turn(agent, msg, msg_len, response_out, response_len_out);
        if (fallback_err == SC_OK && on_token && *response_out && response_len_out && *response_len_out > 0) {
            size_t chunk_size = 8;
            for (size_t i = 0; i < *response_len_out; i += chunk_size) {
                size_t n = *response_len_out - i;
                if (n > chunk_size) n = chunk_size;
                on_token(*response_out + i, n, token_ctx);
            }
        }
        return fallback_err;
    }

    sc_error_t err = append_history(agent, SC_ROLE_USER, msg, msg_len, NULL, 0, NULL, 0);
    if (err != SC_OK) return err;

    char *memory_ctx = NULL;
    size_t memory_ctx_len = 0;
    if (agent->memory && agent->memory->vtable) {
        sc_memory_loader_t loader;
        sc_memory_loader_init(&loader, agent->alloc, agent->memory, 10, 4000);
        (void)sc_memory_loader_load(&loader, msg, msg_len, "", 0, &memory_ctx, &memory_ctx_len);
    }

    char *system_prompt = NULL;
    size_t system_prompt_len = 0;
    if (agent->cached_static_prompt) {
        err = sc_prompt_build_with_cache(agent->alloc,
            agent->cached_static_prompt, agent->cached_static_prompt_len,
            memory_ctx, memory_ctx_len,
            &system_prompt, &system_prompt_len);
        if (memory_ctx) agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (err != SC_OK) return err;
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
        };
        err = sc_prompt_build_system(agent->alloc, &cfg, &system_prompt, &system_prompt_len);
        if (memory_ctx) agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (err != SC_OK) return err;
    }

    sc_chat_message_t *msgs = NULL;
    size_t msgs_count = 0;
    err = sc_context_format_messages(agent->alloc, agent->history, agent->history_count,
        agent->max_history_messages, &msgs, &msgs_count);
    if (err != SC_OK) {
        if (system_prompt) agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
        return err;
    }

    size_t total_msgs = (msgs ? msgs_count : 0) + 1;
    sc_chat_message_t *all_msgs = (sc_chat_message_t *)agent->alloc->alloc(agent->alloc->ctx,
        total_msgs * sizeof(sc_chat_message_t));
    if (!all_msgs) {
        if (system_prompt) agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
        if (msgs) agent->alloc->free(agent->alloc->ctx, msgs, msgs_count * sizeof(sc_chat_message_t));
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
    for (size_t i = 0; i < (msgs ? msgs_count : 0); i++) all_msgs[i + 1] = msgs[i];
    if (msgs) agent->alloc->free(agent->alloc->ctx, msgs, msgs_count * sizeof(sc_chat_message_t));
    msgs = all_msgs;
    msgs_count = total_msgs;

    sc_chat_request_t req;
    req.messages = msgs;
    req.messages_count = msgs_count;
    req.model = agent->model_name;
    req.model_len = agent->model_name_len;
    req.temperature = agent->temperature;
    req.max_tokens = 0;
    req.tools = (agent->tool_specs_count > 0) ? agent->tool_specs : NULL;
    req.tools_count = agent->tool_specs_count;
    req.timeout_secs = 0;
    req.reasoning_effort = NULL;
    req.reasoning_effort_len = 0;

    {
        stream_token_wrap_t wrap = { .on_token = on_token, .token_ctx = token_ctx };
        sc_stream_chat_result_t sresp;
        memset(&sresp, 0, sizeof(sresp));
        err = agent->provider.vtable->stream_chat(agent->provider.ctx, agent->alloc,
            &req, agent->model_name, agent->model_name_len, agent->temperature,
            stream_chunk_to_token_cb, &wrap, &sresp);
        if (msgs) agent->alloc->free(agent->alloc->ctx, msgs, msgs_count * sizeof(sc_chat_message_t));
        if (system_prompt) agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
        if (err != SC_OK) return err;
        agent->total_tokens += sresp.usage.total_tokens;
        if (sresp.content && sresp.content_len > 0) {
            (void)append_history(agent, SC_ROLE_ASSISTANT, sresp.content, sresp.content_len, NULL, 0, NULL, 0);
            *response_out = sc_strndup(agent->alloc, sresp.content, sresp.content_len);
            if (!*response_out) return SC_ERR_OUT_OF_MEMORY;
            if (response_len_out) *response_len_out = sresp.content_len;
        }
        return SC_OK;
    }
}

/* ── Planner execution (Tier 1.4) ────────────────────────────────────── */
sc_error_t sc_agent_execute_plan(sc_agent_t *agent, const char *plan_json, size_t plan_json_len,
    char **summary_out, size_t *summary_len_out)
{
    if (!agent || !plan_json || !summary_out) return SC_ERR_INVALID_ARGUMENT;
    *summary_out = NULL;
    if (summary_len_out) *summary_len_out = 0;

    sc_plan_t *plan = NULL;
    sc_error_t err = sc_planner_create_plan(agent->alloc, plan_json, plan_json_len, &plan);
    if (err != SC_OK) return err;

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
            sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}} };
            ev.data.tool_call_start.tool = plan->steps[i].tool_name;
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        sc_tool_t *tool = find_tool(agent, plan->steps[i].tool_name,
            strlen(plan->steps[i].tool_name));
        if (!tool) {
            sc_planner_mark_step(plan, i, SC_PLAN_STEP_FAILED);
            result_off += snprintf(result_buf + result_off, sizeof(result_buf) - (size_t)result_off,
                "  [%zu] %s: tool not found\n", i + 1, plan->steps[i].tool_name);
            {
                sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TOOL_CALL, .data = {{0}} };
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
            if (pe != SC_OK) args = NULL;
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
            sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TOOL_CALL, .data = {{0}} };
            ev.data.tool_call.tool = plan->steps[i].tool_name;
            ev.data.tool_call.duration_ms = tool_duration_ms;
            ev.data.tool_call.success = ok;
            ev.data.tool_call.detail = ok ? NULL : (result.error_msg ? result.error_msg : "failed");
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        const char *desc = plan->steps[i].description ? plan->steps[i].description : plan->steps[i].tool_name;
        result_off += snprintf(result_buf + result_off, sizeof(result_buf) - (size_t)result_off,
            "  [%zu] %s: %s (%llums)\n", i + 1, desc,
            ok ? "done" : "FAILED", (unsigned long long)tool_duration_ms);

        sc_tool_result_free(agent->alloc, &result);
    }

    sc_plan_free(agent->alloc, plan);

    *summary_out = sc_strndup(agent->alloc, result_buf, (size_t)result_off);
    if (!*summary_out) return SC_ERR_OUT_OF_MEMORY;
    if (summary_len_out) *summary_len_out = (size_t)result_off;
    return SC_OK;
}
