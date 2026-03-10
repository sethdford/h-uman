#include "human/agent/commands.h"
#include "human/agent.h"
#include "human/agent/mailbox.h"
#include "human/agent/planner.h"
#include "human/agent/prompt.h"
#include "human/agent/spawn.h"
#include "human/agent/task_list.h"
#include "human/agent/undo.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local hu_slash_cmd_t g_parsed;
static _Thread_local char g_name_buf[64];
static _Thread_local char g_arg_buf[512];

static int ci_equal(const char *a, size_t na, const char *b, size_t nb) {
    if (na != nb)
        return 0;
    for (size_t i = 0; i < na; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

const hu_slash_cmd_t *hu_agent_commands_parse(const char *msg, size_t len) {
    if (!msg || len < 2 || msg[0] != '/')
        return NULL;
    while (len > 0 && (msg[len - 1] == ' ' || msg[len - 1] == '\r' || msg[len - 1] == '\n'))
        len--;
    const char *body = msg + 1;
    size_t body_len = len - 1;
    if (body_len == 0)
        return NULL;

    size_t split = 0;
    while (split < body_len && body[split] != ':' && body[split] != ' ' && body[split] != '\t')
        split++;
    if (split == 0)
        return NULL;

    const char *raw_name = body;
    size_t name_len = split;
    const char *mention = memchr(raw_name, '@', name_len);
    if (mention)
        name_len = (size_t)(mention - raw_name);

    size_t rest_off = split;
    if (rest_off < body_len && body[rest_off] == ':')
        rest_off++;
    const char *rest = body + rest_off;
    size_t rest_len = body_len - rest_off;
    while (rest_len > 0 && (rest[0] == ' ' || rest[0] == '\t')) {
        rest++;
        rest_len--;
    }

    size_t cn = name_len < sizeof(g_name_buf) - 1 ? name_len : sizeof(g_name_buf) - 1;
    memcpy(g_name_buf, raw_name, cn);
    g_name_buf[cn] = '\0';
    size_t ca = rest_len < sizeof(g_arg_buf) - 1 ? rest_len : sizeof(g_arg_buf) - 1;
    memcpy(g_arg_buf, rest, ca);
    g_arg_buf[ca] = '\0';

    g_parsed.name = g_name_buf;
    g_parsed.name_len = strlen(g_name_buf);
    g_parsed.arg = g_arg_buf;
    g_parsed.arg_len = strlen(g_arg_buf);
    return &g_parsed;
}

static const char BARE_PROMPT[] =
    "A new session was started via /new or /reset. Execute your Session Startup sequence now - "
    "read the required files before responding to the user. Then greet the user in your configured "
    "persona, if one is provided. Be yourself - use your defined voice, mannerisms, and mood. Keep "
    "it to 1-3 sentences and ask what they want to do. If the runtime model differs from "
    "default_model in the system prompt, mention the default model. Do not mention internal steps, "
    "files, tools, or reasoning.";

hu_error_t hu_agent_commands_bare_session_reset_prompt(hu_allocator_t *alloc, const char *msg,
                                                       size_t len, char **out_prompt) {
    if (!alloc || !out_prompt)
        return HU_ERR_INVALID_ARGUMENT;
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse(msg, len);
    if (!cmd) {
        *out_prompt = NULL;
        return HU_OK;
    }
    if (!ci_equal(cmd->name, cmd->name_len, "new", 3) &&
        !ci_equal(cmd->name, cmd->name_len, "reset", 5)) {
        *out_prompt = NULL;
        return HU_OK;
    }
    if (cmd->arg_len != 0) {
        *out_prompt = NULL;
        return HU_OK;
    }
    *out_prompt = hu_strdup(alloc, BARE_PROMPT);
    return *out_prompt ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

void hu_agent_commands_free_owned_tool_calls(hu_allocator_t *alloc, hu_tool_call_t *tcs,
                                             size_t count) {
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
    alloc->free(alloc->ctx, tcs, count * sizeof(hu_tool_call_t));
}

/* ── Slash command handling (extracted from agent.c) ─────────────────────── */

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

static int hu_strncasecmp(const char *a, const char *b, size_t n) {
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

char *hu_agent_handle_slash_command(hu_agent_t *agent, const char *message, size_t message_len) {
    if (!agent || !message || !is_slash_command(message, message_len)) {
        return NULL;
    }
    char cmd_buf[64], arg_buf[192];
    size_t cmd_len, arg_len;
    parse_slash(message, message_len, cmd_buf, sizeof(cmd_buf), &cmd_len, arg_buf, sizeof(arg_buf),
                &arg_len);

    if (cmd_len == 0)
        return NULL;

    if (hu_strncasecmp(cmd_buf, "help", 4) == 0 || hu_strncasecmp(cmd_buf, "commands", 8) == 0) {
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
        return hu_strndup(agent->alloc, help, strlen(help));
    }

    if (hu_strncasecmp(cmd_buf, "quit", 4) == 0 || hu_strncasecmp(cmd_buf, "exit", 4) == 0) {
        return hu_strndup(agent->alloc, "Goodbye.", 8);
    }

    if (hu_strncasecmp(cmd_buf, "clear", 5) == 0 || hu_strncasecmp(cmd_buf, "new", 3) == 0 ||
        hu_strncasecmp(cmd_buf, "reset", 5) == 0) {
        hu_agent_clear_history(agent);
        return hu_strndup(agent->alloc, "History cleared.", 16);
    }

    if (hu_strncasecmp(cmd_buf, "sessions", 8) == 0) {
        return hu_strndup(agent->alloc, "Active sessions:\n- default (current)\n", 42);
    }

    if (hu_strncasecmp(cmd_buf, "kill", 4) == 0) {
        hu_agent_clear_history(agent);
        return hu_strndup(agent->alloc, "Session killed. History cleared.", 33);
    }

    if (hu_strncasecmp(cmd_buf, "retry", 5) == 0) {
        if (agent->history_count > 0) {
            hu_owned_message_t *last = &agent->history[agent->history_count - 1];
            if (last->role != HU_ROLE_ASSISTANT) {
                return hu_strndup(agent->alloc,
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
                hu_agent_commands_free_owned_tool_calls(agent->alloc, last->tool_calls,
                                                        last->tool_calls_count);
                last->tool_calls = NULL;
                last->tool_calls_count = 0;
            }
            agent->history_count--;
        }
        return hu_strndup(agent->alloc, "Last response removed. Send your message again to retry.",
                          57);
    }

    if (hu_strncasecmp(cmd_buf, "model", 5) == 0) {
        if (arg_len > 0) {
            char *old = agent->model_name;
            size_t old_len = agent->model_name_len;
            agent->model_name = hu_strndup(agent->alloc, arg_buf, arg_len);
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
            hu_prompt_config_t pcfg = {
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
            (void)hu_prompt_build_static(agent->alloc, &pcfg, &agent->cached_static_prompt,
                                         &agent->cached_static_prompt_len);
            agent->cached_static_prompt_cap = agent->cached_static_prompt_len;
        }
        return hu_sprintf(agent->alloc, "Model: %.*s", (int)agent->model_name_len,
                          agent->model_name);
    }

    if (hu_strncasecmp(cmd_buf, "status", 6) == 0) {
        const char *prov = agent->provider.vtable->get_name(agent->provider.ctx);
        return hu_sprintf(agent->alloc,
                          "Provider: %s | Model: %.*s | History: %zu messages | Tokens: %llu",
                          prov ? prov : "?", (int)agent->model_name_len, agent->model_name,
                          (size_t)agent->history_count, (unsigned long long)agent->total_tokens);
    }

    if (hu_strncasecmp(cmd_buf, "cost", 4) == 0) {
        return hu_sprintf(agent->alloc,
                          "Tokens used: %llu (est. cost depends on provider pricing)\n"
                          "History: %zu messages",
                          (unsigned long long)agent->total_tokens, (size_t)agent->history_count);
    }

    if (hu_strncasecmp(cmd_buf, "provider", 8) == 0) {
        const char *prov = agent->provider.vtable->get_name(agent->provider.ctx);
        if (arg_len > 0) {
            return hu_sprintf(agent->alloc,
                              "Provider switching requires restart. Current: %s\n"
                              "Set in config: default_provider = \"%.*s\"",
                              prov ? prov : "?", (int)arg_len, arg_buf);
        }
        return hu_sprintf(agent->alloc, "Provider: %s", prov ? prov : "?");
    }

    if (hu_strncasecmp(cmd_buf, "tools", 5) == 0) {
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

    if (hu_strncasecmp(cmd_buf, "plan", 4) == 0) {
        if (arg_len == 0) {
            return hu_strndup(agent->alloc,
                              "Usage: /plan {\"steps\": [{\"tool\": \"name\", \"args\": {...}}]}",
                              57);
        }
        char *summary = NULL;
        size_t summary_len = 0;
        hu_error_t err = hu_agent_execute_plan(agent, arg_buf, arg_len, &summary, &summary_len);
        if (err != HU_OK) {
            return hu_sprintf(agent->alloc, "Plan failed: %s", hu_error_string(err));
        }
        return summary;
    }

    if (hu_strncasecmp(cmd_buf, "goal", 4) == 0) {
        if (arg_len == 0) {
            return hu_strndup(agent->alloc, "Usage: /goal <describe what you want to accomplish>",
                              51);
        }
        const char **tool_names = NULL;
        size_t tn_count = 0;
        if (agent->tools_count > 0) {
            tool_names = (const char **)agent->alloc->alloc(
                agent->alloc->ctx, agent->tools_count * sizeof(const char *));
            if (tool_names) {
                for (size_t i = 0; i < agent->tools_count; i++) {
                    const char *tn = agent->tools[i].vtable->name
                                         ? agent->tools[i].vtable->name(agent->tools[i].ctx)
                                         : NULL;
                    if (tn)
                        tool_names[tn_count++] = tn;
                }
            }
        }
        hu_plan_t *plan = NULL;
        hu_error_t err = hu_planner_generate(agent->alloc, &agent->provider, agent->model_name,
                                             agent->model_name_len, arg_buf, arg_len, tool_names,
                                             tn_count, &plan);
        if (tool_names)
            agent->alloc->free(agent->alloc->ctx, (void *)tool_names,
                               agent->tools_count * sizeof(const char *));
        if (err != HU_OK || !plan) {
            return hu_sprintf(agent->alloc, "Goal planning failed: %s", hu_error_string(err));
        }
        char *summary = NULL;
        size_t summary_len = 0;
        err = hu_agent_commands_execute_plan_steps(agent, plan, &summary, &summary_len, arg_buf,
                                                   arg_len);
        hu_plan_free(agent->alloc, plan);
        if (err != HU_OK)
            return hu_sprintf(agent->alloc, "Plan execution failed: %s", hu_error_string(err));
        return summary;
    }

    if (hu_strncasecmp(cmd_buf, "spawn", 5) == 0) {
        if (!agent->agent_pool)
            return hu_strndup(agent->alloc, "Agent pool not configured.", 26);
        if (arg_len == 0)
            return hu_strndup(agent->alloc, "Usage: /spawn <task description>", 32);
        hu_spawn_config_t scfg;
        memset(&scfg, 0, sizeof(scfg));
        scfg.mode = HU_SPAWN_ONE_SHOT;
        scfg.max_iterations = 10;
        scfg.mailbox = agent->mailbox;
        if (agent->persona_name) {
            scfg.persona_name = agent->persona_name;
            scfg.persona_name_len = agent->persona_name_len;
        }
        uint64_t new_id = 0;
        hu_error_t err =
            hu_agent_pool_spawn(agent->agent_pool, &scfg, arg_buf, arg_len, "cli-spawn", &new_id);
        if (err != HU_OK)
            return hu_sprintf(agent->alloc, "Spawn failed: %s", hu_error_string(err));
        return hu_sprintf(agent->alloc, "Spawned agent #%llu", (unsigned long long)new_id);
    }

    if (hu_strncasecmp(cmd_buf, "agents", 6) == 0) {
        if (!agent->agent_pool)
            return hu_strndup(agent->alloc, "Agent pool not configured.", 26);
        hu_agent_pool_info_t *info = NULL;
        size_t info_count = 0;
        hu_error_t err = hu_agent_pool_list(agent->agent_pool, agent->alloc, &info, &info_count);
        if (err != HU_OK)
            return hu_sprintf(agent->alloc, "List failed: %s", hu_error_string(err));
        if (info_count == 0) {
            if (info)
                agent->alloc->free(agent->alloc->ctx, info, 0);
            return hu_strndup(agent->alloc, "No agents running.", 18);
        }
        char *buf = (char *)agent->alloc->alloc(agent->alloc->ctx, 4096);
        if (!buf) {
            agent->alloc->free(agent->alloc->ctx, info, info_count * sizeof(hu_agent_pool_info_t));
            return NULL;
        }
        int off = snprintf(buf, 4096, "Agents (%zu):\n", info_count);
        for (size_t i = 0; i < info_count && off < 4000; i++) {
            off += snprintf(buf + off, 4096 - (size_t)off, "  #%llu [%s] %s\n",
                            (unsigned long long)info[i].agent_id,
                            info[i].status == HU_AGENT_RUNNING     ? "running"
                            : info[i].status == HU_AGENT_IDLE      ? "idle"
                            : info[i].status == HU_AGENT_COMPLETED ? "done"
                            : info[i].status == HU_AGENT_FAILED    ? "failed"
                                                                   : "cancelled",
                            info[i].label ? info[i].label : "");
        }
        agent->alloc->free(agent->alloc->ctx, info, info_count * sizeof(hu_agent_pool_info_t));
        return buf;
    }

    if (hu_strncasecmp(cmd_buf, "cancel", 6) == 0) {
        if (!agent->agent_pool)
            return hu_strndup(agent->alloc, "Agent pool not configured.", 26);
        if (arg_len == 0)
            return hu_strndup(agent->alloc, "Usage: /cancel <agent-id>", 25);
        uint64_t cid = (uint64_t)strtoull(arg_buf, NULL, 10);
        hu_error_t err = hu_agent_pool_cancel(agent->agent_pool, cid);
        if (err != HU_OK)
            return hu_sprintf(agent->alloc, "Cancel failed: %s", hu_error_string(err));
        return hu_sprintf(agent->alloc, "Cancelled agent #%llu", (unsigned long long)cid);
    }

    if (hu_strncasecmp(cmd_buf, "send", 4) == 0) {
        if (!agent->mailbox)
            return hu_strndup(agent->alloc, "Mailbox not configured.", 22);
        if (arg_len == 0)
            return hu_strndup(agent->alloc, "Usage: /send <agent-id> <message>", 32);
        const char *p = arg_buf;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return hu_strndup(agent->alloc, "Usage: /send <agent-id> <message>", 32);
        uint64_t to_id = (uint64_t)strtoull(p, (char **)&p, 10);
        while (*p == ' ' || *p == '\t')
            p++;
        uint64_t from_id = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
        hu_error_t err =
            hu_mailbox_send(agent->mailbox, from_id, to_id, HU_MSG_TASK, p, strlen(p), 0);
        if (err != HU_OK)
            return hu_sprintf(agent->alloc, "Send failed: %s", hu_error_string(err));
        return hu_sprintf(agent->alloc, "Sent to agent #%llu", (unsigned long long)to_id);
    }

    if (hu_strncasecmp(cmd_buf, "tasks", 5) == 0) {
        if (!agent->task_list)
            return hu_strndup(agent->alloc, "Task list not configured.", 25);
        size_t pending = hu_task_list_count_by_status(agent->task_list, HU_TASK_LIST_PENDING);
        size_t claimed = hu_task_list_count_by_status(agent->task_list, HU_TASK_LIST_CLAIMED);
        size_t completed = hu_task_list_count_by_status(agent->task_list, HU_TASK_LIST_COMPLETED);
        return hu_sprintf(agent->alloc, "Tasks: %zu pending, %zu claimed, %zu completed", pending,
                          claimed, completed);
    }

    if (hu_strncasecmp(cmd_buf, "task", 4) == 0) {
        if (!agent->task_list)
            return hu_strndup(agent->alloc, "Task list not configured.", 25);
        const char *p = arg_buf;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return hu_strndup(agent->alloc, "Usage: /task add <subject> | claim <id> | done <id>",
                              52);
        if (hu_strncasecmp(p, "add", 3) == 0) {
            p += 3;
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p)
                return hu_strndup(agent->alloc, "Usage: /task add <subject>", 26);
            uint64_t id = 0;
            hu_error_t err = hu_task_list_add(agent->task_list, p, NULL, NULL, 0, &id);
            if (err != HU_OK)
                return hu_sprintf(agent->alloc, "Add failed: %s", hu_error_string(err));
            return hu_sprintf(agent->alloc, "Task #%llu created", (unsigned long long)id);
        }
        if (hu_strncasecmp(p, "claim", 5) == 0) {
            p += 5;
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p)
                return hu_strndup(agent->alloc, "Usage: /task claim <id>", 23);
            uint64_t tid = (uint64_t)strtoull(p, NULL, 10);
            uint64_t aid = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
            hu_error_t err = hu_task_list_claim(agent->task_list, tid, aid);
            if (err != HU_OK)
                return hu_sprintf(agent->alloc, "Claim failed: %s", hu_error_string(err));
            return hu_sprintf(agent->alloc, "Claimed task #%llu", (unsigned long long)tid);
        }
        if (hu_strncasecmp(p, "done", 4) == 0) {
            p += 4;
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p)
                return hu_strndup(agent->alloc, "Usage: /task done <id>", 22);
            uint64_t tid = (uint64_t)strtoull(p, NULL, 10);
            hu_error_t err =
                hu_task_list_update_status(agent->task_list, tid, HU_TASK_LIST_COMPLETED);
            if (err != HU_OK)
                return hu_sprintf(agent->alloc, "Done failed: %s", hu_error_string(err));
            return hu_sprintf(agent->alloc, "Task #%llu completed", (unsigned long long)tid);
        }
        return hu_strndup(agent->alloc, "Usage: /task add <subject> | claim <id> | done <id>", 52);
    }

    if (hu_strncasecmp(cmd_buf, "undo", 4) == 0) {
        if (!agent->undo_stack)
            return hu_strndup(agent->alloc, "Undo stack not configured.", 27);
        hu_error_t err = hu_undo_stack_execute_undo(agent->undo_stack, agent->alloc);
        if (err != HU_OK)
            return hu_sprintf(agent->alloc, "Undo failed: %s", hu_error_string(err));
        return hu_strndup(agent->alloc, "Undone.", 7);
    }

    return NULL;
}
