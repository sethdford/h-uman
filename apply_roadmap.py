#!/usr/bin/env python3
"""
Mega-patch: Apply all roadmap wiring changes atomically.
Run from repo root: python3 apply_roadmap.py
"""
import subprocess, sys, os

def patch(path, pairs):
    """Apply string replacements to a file. Only replaces if old text found."""
    with open(path, 'r') as f:
        c = f.read()
    for old, new in pairs:
        if old in c:
            c = c.replace(old, new, 1)
    with open(path, 'w') as f:
        f.write(c)

def has(path, key):
    with open(path) as f:
        return key in f.read()

# ════════════════════════════════════════════════════════════════════
# 1. HEADER PATCHES (keep reverting — apply every time)
# ════════════════════════════════════════════════════════════════════

# agent.h: add pool/mailbox/policy_engine fields
patch('include/seaclaw/agent.h', [
    ('#include "seaclaw/voice.h"',
     '#include "seaclaw/voice.h"\n#include "seaclaw/agent/spawn.h"\n#include "seaclaw/agent/mailbox.h"\n#include "seaclaw/security/policy_engine.h"'),
    ('    sc_arena_t *turn_arena; /* per-turn bump allocator for ephemeral allocations */\n};',
     '    sc_arena_t *turn_arena; /* per-turn bump allocator for ephemeral allocations */\n\n    sc_agent_pool_t *agent_pool;\n    sc_mailbox_t *mailbox;\n    sc_policy_engine_t *policy_engine;\n};'),
])

# config.h: add pool_max_concurrent, default_profile, policy/plugins config
patch('include/seaclaw/config.h', [
    ('    uint64_t message_timeout_secs;\n} sc_agent_config_t;',
     '    uint64_t message_timeout_secs;\n    uint32_t pool_max_concurrent;\n    char *default_profile;\n} sc_agent_config_t;\n\ntypedef struct sc_policy_config {\n    bool enabled;\n    char *rules_json;\n} sc_policy_config_t;\n\ntypedef struct sc_plugins_config {\n    bool enabled;\n    char *plugin_dir;\n} sc_plugins_config_t;'),
    ('    size_t mcp_servers_len;\n    sc_arena_t *arena;',
     '    size_t mcp_servers_len;\n    sc_policy_config_t policy;\n    sc_plugins_config_t plugins;\n    sc_arena_t *arena;'),
])

# seaclaw.h: umbrella includes
patch('include/seaclaw/seaclaw.h', [
    ('#endif /* SC_SEACLAW_H */',
     '\n#include "agent/spawn.h"\n#include "agent/mailbox.h"\n#include "agent/profile.h"\n#include "channels/thread_binding.h"\n#include "security/policy_engine.h"\n#include "security/replay.h"\n#include "observability/otel.h"\n#include "plugin.h"\n\n#endif /* SC_SEACLAW_H */'),
])

# factory.h: add agent_pool param
patch('include/seaclaw/tools/factory.h', [
    ('#include "seaclaw/tool.h"\n#include <stddef.h>',
     '#include "seaclaw/tool.h"\n#include "seaclaw/agent/spawn.h"\n#include <stddef.h>'),
    ('    sc_cron_scheduler_t *cron,\n    sc_tool_t **out_tools',
     '    sc_cron_scheduler_t *cron,\n    sc_agent_pool_t *agent_pool,\n    sc_tool_t **out_tools'),
])

# ════════════════════════════════════════════════════════════════════
# 2. CONFIG.C: defaults + parse functions
# ════════════════════════════════════════════════════════════════════

patch('src/config.c', [
    # Add defaults
    ('    cfg->agent.message_timeout_secs = 600;\n    cfg->reliability.provider_retries = 2;',
     '    cfg->agent.message_timeout_secs = 600;\n    cfg->agent.pool_max_concurrent = 8;\n    cfg->agent.default_profile = NULL;\n    cfg->policy.enabled = false;\n    cfg->policy.rules_json = NULL;\n    cfg->plugins.enabled = false;\n    cfg->plugins.plugin_dir = NULL;\n    cfg->reliability.provider_retries = 2;'),
    # Add parse_agent pool fields + new parse functions
    ('    if (mts >= 0) cfg->agent.message_timeout_secs = (uint64_t)mts;\n    return SC_OK;\n}\n\nstatic sc_error_t parse_heartbeat(',
     '    if (mts >= 0) cfg->agent.message_timeout_secs = (uint64_t)mts;\n    double pmc = sc_json_get_number(obj, "pool_max_concurrent", cfg->agent.pool_max_concurrent);\n    if (pmc >= 1 && pmc <= 64) cfg->agent.pool_max_concurrent = (uint32_t)pmc;\n    const char *dp = sc_json_get_string(obj, "default_profile");\n    if (dp) {\n        if (cfg->agent.default_profile) a->free(a->ctx, cfg->agent.default_profile, strlen(cfg->agent.default_profile) + 1);\n        cfg->agent.default_profile = sc_strdup(a, dp);\n    }\n    return SC_OK;\n}\n\nstatic sc_error_t parse_policy_cfg(sc_allocator_t *a, sc_config_t *cfg,\n                                   const sc_json_value_t *obj) {\n    (void)a;\n    if (!obj || obj->type != SC_JSON_OBJECT) return SC_OK;\n    cfg->policy.enabled = sc_json_get_bool(obj, "enabled", cfg->policy.enabled);\n    return SC_OK;\n}\n\nstatic sc_error_t parse_plugins_cfg(sc_allocator_t *a, sc_config_t *cfg,\n                                    const sc_json_value_t *obj) {\n    (void)a;\n    if (!obj || obj->type != SC_JSON_OBJECT) return SC_OK;\n    cfg->plugins.enabled = sc_json_get_bool(obj, "enabled", cfg->plugins.enabled);\n    return SC_OK;\n}\n\nstatic sc_error_t parse_heartbeat('),
    # Wire parse functions into main parser
    ('    if (mcp_obj) parse_mcp_servers(a, cfg, mcp_obj);\n\n    sc_json_value_t *sec = sc_json_object_get(root, "security");',
     '    if (mcp_obj) parse_mcp_servers(a, cfg, mcp_obj);\n\n    sc_json_value_t *policy_obj = sc_json_object_get(root, "policy");\n    if (policy_obj) parse_policy_cfg(a, cfg, policy_obj);\n    sc_json_value_t *plugins_obj = sc_json_object_get(root, "plugins");\n    if (plugins_obj) parse_plugins_cfg(a, cfg, plugins_obj);\n\n    sc_json_value_t *sec = sc_json_object_get(root, "security");'),
])

# ════════════════════════════════════════════════════════════════════
# 3. FACTORY.C: wire agent_pool into tools
# ════════════════════════════════════════════════════════════════════

patch('src/tools/factory.c', [
    ('sc_error_t sc_tools_create_default(sc_allocator_t *alloc,\n    const char *workspace_dir, size_t workspace_dir_len,\n    sc_security_policy_t *policy,\n    const sc_config_t *config,\n    sc_memory_t *memory,\n    sc_cron_scheduler_t *cron,\n    sc_tool_t **out_tools, size_t *out_count)',
     'sc_error_t sc_tools_create_default(sc_allocator_t *alloc,\n    const char *workspace_dir, size_t workspace_dir_len,\n    sc_security_policy_t *policy,\n    const sc_config_t *config,\n    sc_memory_t *memory,\n    sc_cron_scheduler_t *cron,\n    sc_agent_pool_t *agent_pool,\n    sc_tool_t **out_tools, size_t *out_count)'),
    ('    err = sc_agent_query_tool_create(alloc, NULL, &tools[idx]);',
     '    err = sc_agent_query_tool_create(alloc, agent_pool, &tools[idx]);'),
    ('    err = sc_agent_spawn_tool_create(alloc, NULL, &tools[idx]);',
     '    err = sc_agent_spawn_tool_create(alloc, agent_pool, &tools[idx]);'),
])

# ════════════════════════════════════════════════════════════════════
# 4. AGENT.C: slash commands + policy check
# ════════════════════════════════════════════════════════════════════

if not has('src/agent/agent.c', '/spawn'):
    patch('src/agent/agent.c', [
        # Update help text
        ('"  /status           Show agent status\\n";',
         '"  /status           Show agent status\\n"\n'
         '            "  /spawn <task>     Spawn a sub-agent\\n"\n'
         '            "  /agents           List running agents\\n"\n'
         '            "  /cancel <id>      Cancel a sub-agent\\n";'),
    ])

    with open('src/agent/agent.c', 'r') as f:
        c = f.read()

    # Add slash commands + policy check helper before find_tool
    old_ret = '    return NULL;\n}\n\nstatic sc_tool_t *find_tool('
    new_cmds = """    if (sc_strncasecmp(cmd_buf, "spawn", 5) == 0) {
        if (!agent->agent_pool)
            return sc_strndup(agent->alloc, "Agent pool not configured.", 26);
        if (arg_len == 0)
            return sc_strndup(agent->alloc, "Usage: /spawn <task description>", 32);
        sc_spawn_config_t scfg;
        memset(&scfg, 0, sizeof(scfg));
        scfg.mode = SC_SPAWN_ONE_SHOT;
        scfg.max_iterations = 10;
        uint64_t new_id = 0;
        sc_error_t err = sc_agent_pool_spawn(agent->agent_pool, &scfg, arg_buf, arg_len, "cli-spawn", &new_id);
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
            if (info) agent->alloc->free(agent->alloc->ctx, info, 0);
            return sc_strndup(agent->alloc, "No agents running.", 18);
        }
        char *buf = (char *)agent->alloc->alloc(agent->alloc->ctx, 4096);
        if (!buf) { agent->alloc->free(agent->alloc->ctx, info, info_count * sizeof(sc_agent_pool_info_t)); return NULL; }
        int off = snprintf(buf, 4096, "Agents (%zu):\\n", info_count);
        for (size_t i = 0; i < info_count && off < 4000; i++) {
            off += snprintf(buf + off, 4096 - (size_t)off, "  #%llu [%s] %s\\n",
                (unsigned long long)info[i].agent_id,
                info[i].status == SC_AGENT_RUNNING ? "running" :
                info[i].status == SC_AGENT_IDLE ? "idle" :
                info[i].status == SC_AGENT_COMPLETED ? "done" :
                info[i].status == SC_AGENT_FAILED ? "failed" : "cancelled",
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

    return NULL;
}

"""

    policy_helper = """static sc_policy_action_t sc_agent_check_policy(sc_agent_t *agent,
    const char *tool_name, const char *arguments)
{
    if (!agent->policy_engine) return SC_POLICY_ALLOW;
    sc_policy_eval_ctx_t pe_ctx = { .tool_name = tool_name, .args_json = arguments, .session_cost_usd = 0 };
    sc_policy_result_t pe_res = sc_policy_engine_evaluate(agent->policy_engine, &pe_ctx);
    return pe_res.action;
}

static sc_tool_t *find_tool("""

    c = c.replace(old_ret, new_cmds + policy_helper)

    # Add policy check at sequential tool dispatch
    c = c.replace(
        '                    sc_tool_result_t result = sc_tool_result_fail("invalid arguments", 16);\n                    if (args) {\n                        tool->vtable->execute(tool->ctx, agent->alloc, args, &result);\n                        sc_json_free(agent->alloc, args);\n                    }\n\n                    /* Approval retry for sequential fallback path */',
        '                    sc_tool_result_t result = sc_tool_result_fail("invalid arguments", 16);\n                    if (args) {\n                        char pol_tn[64];\n                        size_t pol_tn_len = call->name_len < sizeof(pol_tn) - 1 ? call->name_len : sizeof(pol_tn) - 1;\n                        if (pol_tn_len > 0 && call->name) memcpy(pol_tn, call->name, pol_tn_len);\n                        pol_tn[pol_tn_len] = \'\\0\';\n                        sc_policy_action_t pa = sc_agent_check_policy(agent, pol_tn, call->arguments);\n                        if (pa == SC_POLICY_DENY) {\n                            result = sc_tool_result_fail("denied by policy", 16);\n                            sc_json_free(agent->alloc, args);\n                        } else {\n                            tool->vtable->execute(tool->ctx, agent->alloc, args, &result);\n                            sc_json_free(agent->alloc, args);\n                        }\n                    }\n\n                    /* Approval retry for sequential fallback path */')

    with open('src/agent/agent.c', 'w') as f:
        f.write(c)

# ════════════════════════════════════════════════════════════════════
# 5. CLI.C: wire pool, mailbox, policy engine, profiles, OTel
# ════════════════════════════════════════════════════════════════════

if not has('src/agent/cli.c', 'cli_agent_pool'):
    patch('src/agent/cli.c', [
        # Add includes
        ('#include "seaclaw/cron.h"',
         '#include "seaclaw/cron.h"\n#include "seaclaw/agent/profile.h"\n#include "seaclaw/observability/otel.h"\n#include "seaclaw/plugin.h"'),
        # Create pool + mailbox before tools
        ('    sc_cron_scheduler_t *cron = sc_cron_create(alloc, 64, true);\n\n    sc_tool_t *tools = NULL;\n    size_t tools_count = 0;\n    err = sc_tools_create_default(alloc, ws, strlen(ws), &policy, &cfg,\n        memory.vtable ? &memory : NULL, cron, NULL, &tools, &tools_count);',
         '    sc_cron_scheduler_t *cron = sc_cron_create(alloc, 64, true);\n\n    sc_agent_pool_t *cli_agent_pool = sc_agent_pool_create(alloc, cfg.agent.pool_max_concurrent);\n    sc_mailbox_t *cli_mailbox = sc_mailbox_create(alloc, 64);\n\n    sc_tool_t *tools = NULL;\n    size_t tools_count = 0;\n    err = sc_tools_create_default(alloc, ws, strlen(ws), &policy, &cfg,\n        memory.vtable ? &memory : NULL, cron, cli_agent_pool, &tools, &tools_count);'),
        # Wire into agent + create policy engine + OTel + profiles
        ('    sc_agent_set_retrieval_engine(&agent, &retrieval_engine);\n\n    /* TUI mode',
         '    agent.agent_pool = cli_agent_pool;\n    agent.mailbox = cli_mailbox;\n    agent.policy_engine = NULL;\n\n    if (cfg.policy.enabled) {\n        agent.policy_engine = sc_policy_engine_create(alloc);\n    }\n\n    if (cfg.agent.default_profile) {\n        const sc_agent_profile_t *prof = sc_agent_profile_by_name(cfg.agent.default_profile, strlen(cfg.agent.default_profile));\n        if (prof) {\n            if (prof->preferred_model && prof->preferred_model[0] && !parsed_args.model) {\n                char *old = agent.model_name;\n                size_t old_len = agent.model_name_len;\n                agent.model_name = sc_strndup(alloc, prof->preferred_model, strlen(prof->preferred_model));\n                agent.model_name_len = strlen(prof->preferred_model);\n                if (old) alloc->free(alloc->ctx, old, old_len + 1);\n            }\n            if (prof->temperature > 0) agent.temperature = prof->temperature;\n            if (prof->max_iterations > 0) agent.max_tool_iterations = prof->max_iterations;\n            if (prof->max_history > 0) agent.max_history_messages = prof->max_history;\n        }\n    }\n\n    sc_observer_t otel_observer = {0};\n    if (cfg.diagnostics.otel_endpoint && cfg.diagnostics.otel_endpoint[0]) {\n        sc_otel_config_t otel_cfg = {\n            .endpoint = cfg.diagnostics.otel_endpoint,\n            .endpoint_len = strlen(cfg.diagnostics.otel_endpoint),\n            .service_name = cfg.diagnostics.otel_service_name ? cfg.diagnostics.otel_service_name : "seaclaw",\n            .service_name_len = cfg.diagnostics.otel_service_name ? strlen(cfg.diagnostics.otel_service_name) : 7,\n            .enable_traces = true, .enable_metrics = true, .enable_logs = true,\n        };\n        if (sc_otel_observer_create(alloc, &otel_cfg, &otel_observer) == SC_OK && otel_observer.vtable) {\n            agent.observer = &otel_observer;\n        }\n    }\n\n    sc_agent_set_retrieval_engine(&agent, &retrieval_engine);\n\n    /* TUI mode'),
        # TUI cleanup: add pool/mailbox/policy destroy
        ('        sc_tools_destroy_default(alloc, tools, tools_count);\n        if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);\n        if (sb_storage) sc_sandbox_storage_destroy(sb_storage, &sb_alloc);\n        sc_config_deinit(&cfg);\n        return err;\n    }\n\n    /* Install SIGINT handler */',
         '        sc_tools_destroy_default(alloc, tools, tools_count);\n        if (otel_observer.vtable && otel_observer.vtable->deinit) otel_observer.vtable->deinit(otel_observer.ctx);\n        if (agent.policy_engine) sc_policy_engine_destroy(agent.policy_engine);\n        if (cli_mailbox) sc_mailbox_destroy(cli_mailbox);\n        if (cli_agent_pool) sc_agent_pool_destroy(cli_agent_pool);\n        if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);\n        if (sb_storage) sc_sandbox_storage_destroy(sb_storage, &sb_alloc);\n        sc_config_deinit(&cfg);\n        return err;\n    }\n\n    /* Install SIGINT handler */'),
        # REPL cleanup
        ('    sc_tools_destroy_default(alloc, tools, tools_count);\n    if (cron) sc_cron_destroy(cron, alloc);\n    if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);\n    if (sb_storage) sc_sandbox_storage_destroy(sb_storage, &sb_alloc);\n    sc_config_deinit(&cfg);\n    return SC_OK;\n}',
         '    sc_tools_destroy_default(alloc, tools, tools_count);\n    if (otel_observer.vtable && otel_observer.vtable->deinit) otel_observer.vtable->deinit(otel_observer.ctx);\n    if (agent.policy_engine) sc_policy_engine_destroy(agent.policy_engine);\n    if (cli_mailbox) sc_mailbox_destroy(cli_mailbox);\n    if (cli_agent_pool) sc_agent_pool_destroy(cli_agent_pool);\n    if (cron) sc_cron_destroy(cron, alloc);\n    if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);\n    if (sb_storage) sc_sandbox_storage_destroy(sb_storage, &sb_alloc);\n    sc_config_deinit(&cfg);\n    return SC_OK;\n}'),
    ])

# ════════════════════════════════════════════════════════════════════
# 6. MAIN.C: wire pool/mailbox/policy/OTel/thread_binding in all modes
# ════════════════════════════════════════════════════════════════════

if not has('src/main.c', 'svc_mailbox'):
    patch('src/main.c', [
        # cmd_service_loop: create pool + mailbox
        ('    sc_cron_scheduler_t *cron = sc_cron_create(alloc, 64, true);\n\n    sc_tool_t *tools = NULL;\n    size_t tools_count = 0;\n    err = sc_tools_create_default(alloc, ws, strlen(ws), &policy, &cfg,\n        memory.vtable ? &memory : NULL, cron, agent_pool, &tools, &tools_count);',
         '    sc_cron_scheduler_t *cron = sc_cron_create(alloc, 64, true);\n\n    sc_agent_pool_t *agent_pool = sc_agent_pool_create(alloc, cfg.agent.pool_max_concurrent);\n    sc_mailbox_t *svc_mailbox = sc_mailbox_create(alloc, 64);\n\n    sc_tool_t *tools = NULL;\n    size_t tools_count = 0;\n    err = sc_tools_create_default(alloc, ws, strlen(ws), &policy, &cfg,\n        memory.vtable ? &memory : NULL, cron, agent_pool, &tools, &tools_count);'),
        # Wire into agent struct
        ('    sc_agent_set_retrieval_engine(&agent, &retrieval_engine);\n\n    fprintf(stderr, "[%s] agent ready',
         '    agent.agent_pool = agent_pool;\n    agent.mailbox = svc_mailbox;\n    agent.policy_engine = NULL;\n    if (cfg.policy.enabled) agent.policy_engine = sc_policy_engine_create(alloc);\n    sc_agent_set_retrieval_engine(&agent, &retrieval_engine);\n\n    fprintf(stderr, "[%s] agent ready'),
        # Cleanup
        ('    sc_agent_deinit(&agent);\n    sc_tools_destroy_default(alloc, tools, tools_count);\n    if (cron) sc_cron_destroy(cron, alloc);\n    if (retrieval_engine',
         '    sc_agent_deinit(&agent);\n    sc_tools_destroy_default(alloc, tools, tools_count);\n    if (agent.policy_engine) sc_policy_engine_destroy(agent.policy_engine);\n    if (svc_mailbox) sc_mailbox_destroy(svc_mailbox);\n    if (agent_pool) sc_agent_pool_destroy(agent_pool);\n    if (cron) sc_cron_destroy(cron, alloc);\n    if (retrieval_engine'),
    ])

# cmd_gateway: create pool
if not has('src/main.c', 'gw_agent_pool'):
    patch('src/main.c', [
        ('    sc_tool_t *tools = NULL;\n    size_t tools_count = 0;\n    err = sc_tools_create_default(alloc, ws, strlen(ws), &policy, &cfg,\n        NULL, cron, &tools, &tools_count);\n    if (err != SC_OK) {\n        fprintf(stderr, "[%s] Tools init failed: %s\\n", SC_CODENAME, sc_error_string(err));\n        sc_cost_tracker_deinit(&costs);\n        sc_session_manager_deinit(&sessions);\n        if (cron) sc_cron_destroy(cron, alloc);\n        sc_skillforge_destroy(&skills);\n        if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);\n        if (gw_sb_storage) sc_sandbox_storage_destroy(gw_sb_storage, &gw_sb_alloc);\n        sc_config_deinit(&cfg);\n        return err;\n    }',
         '    sc_agent_pool_t *gw_agent_pool = sc_agent_pool_create(alloc, cfg.agent.pool_max_concurrent);\n    sc_thread_binding_t *gw_thread_binding = sc_thread_binding_create(alloc, 64);\n\n    sc_tool_t *tools = NULL;\n    size_t tools_count = 0;\n    err = sc_tools_create_default(alloc, ws, strlen(ws), &policy, &cfg,\n        NULL, cron, gw_agent_pool, &tools, &tools_count);\n    if (err != SC_OK) {\n        fprintf(stderr, "[%s] Tools init failed: %s\\n", SC_CODENAME, sc_error_string(err));\n        sc_cost_tracker_deinit(&costs);\n        sc_session_manager_deinit(&sessions);\n        if (cron) sc_cron_destroy(cron, alloc);\n        sc_skillforge_destroy(&skills);\n        if (gw_agent_pool) sc_agent_pool_destroy(gw_agent_pool);\n        if (gw_thread_binding) sc_thread_binding_destroy(gw_thread_binding);\n        if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);\n        if (gw_sb_storage) sc_sandbox_storage_destroy(gw_sb_storage, &gw_sb_alloc);\n        sc_config_deinit(&cfg);\n        return err;\n    }'),
        # Wire pool into gateway agent
        ('        agent_bridge.agent = &agent;\n        agent_bridge.bus = &bus;',
         '        agent.agent_pool = gw_agent_pool;\n        agent.policy_engine = NULL;\n        if (cfg.policy.enabled) agent.policy_engine = sc_policy_engine_create(alloc);\n        agent_bridge.agent = &agent;\n        agent_bridge.bus = &bus;'),
        # Gateway cleanup
        ('    sc_cost_tracker_deinit(&costs);\n    sc_session_manager_deinit(&sessions);\n    if (cron) sc_cron_destroy(cron, alloc);\n    sc_skillforge_destroy(&skills);\n    sc_config_deinit(&cfg);\n    return err;\n}\n\nstatic int run_command(',
         '    sc_cost_tracker_deinit(&costs);\n    sc_session_manager_deinit(&sessions);\n    if (gw_agent_pool) sc_agent_pool_destroy(gw_agent_pool);\n    if (gw_thread_binding) sc_thread_binding_destroy(gw_thread_binding);\n    if (cron) sc_cron_destroy(cron, alloc);\n    sc_skillforge_destroy(&skills);\n    sc_config_deinit(&cfg);\n    return err;\n}\n\nstatic int run_command('),
    ])

# cmd_mcp: add pool
if not has('src/main.c', 'mcp_pool'):
    patch('src/main.c', [
        ('    sc_tool_t *tools = NULL;\n    size_t tool_count = 0;\n    err = sc_tools_create_default(alloc, ".", 1, NULL, &cfg, NULL, NULL,\n        &tools, &tool_count);',
         '    sc_agent_pool_t *mcp_pool = sc_agent_pool_create(alloc, cfg.agent.pool_max_concurrent);\n\n    sc_tool_t *tools = NULL;\n    size_t tool_count = 0;\n    err = sc_tools_create_default(alloc, ".", 1, NULL, &cfg, NULL, NULL,\n        mcp_pool, &tools, &tool_count);'),
        ('    sc_mcp_host_destroy(srv);\n    sc_tools_destroy_default(alloc, tools, tool_count);\n    sc_config_deinit(&cfg);\n    return err;\n}',
         '    sc_mcp_host_destroy(srv);\n    sc_tools_destroy_default(alloc, tools, tool_count);\n    if (mcp_pool) sc_agent_pool_destroy(mcp_pool);\n    sc_config_deinit(&cfg);\n    return err;\n}'),
    ])

# ════════════════════════════════════════════════════════════════════
# 7. INTEGRATION TESTS
# ════════════════════════════════════════════════════════════════════

path = 'tests/test_roadmap.c'
with open(path, 'r') as f:
    c = f.read()

if 'test_integ_config_defaults' not in c:
    if '#include "seaclaw/tools/factory.h"' not in c:
        c = c.replace('#include "seaclaw/agent/profile.h"',
            '#include "seaclaw/agent/profile.h"\n#include "seaclaw/tools/factory.h"\n#include "seaclaw/config.h"\n#include "seaclaw/agent.h"')

    integ = r'''
static void test_integ_config_defaults(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_error_t err = sc_config_load(&a, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(cfg.agent.pool_max_concurrent, 8);
    SC_ASSERT_NULL(cfg.agent.default_profile);
    SC_ASSERT_EQ(cfg.policy.enabled, false);
    SC_ASSERT_EQ(cfg.plugins.enabled, false);
    sc_config_deinit(&cfg);
}

static void test_integ_config_parse_pool(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_error_t err = sc_config_load(&a, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
    const char *json = "{\"agent\":{\"pool_max_concurrent\":16}}";
    err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(cfg.agent.pool_max_concurrent, 16);
    sc_config_deinit(&cfg);
}

static void test_integ_config_parse_policy(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_error_t err = sc_config_load(&a, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
    const char *json = "{\"policy\":{\"enabled\":true}}";
    err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(cfg.policy.enabled, true);
    sc_config_deinit(&cfg);
}

static void test_integ_agent_fields(void) {
    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    SC_ASSERT_NULL(agent.agent_pool);
    SC_ASSERT_NULL(agent.mailbox);
    SC_ASSERT_NULL(agent.policy_engine);
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *pool = sc_agent_pool_create(&a, 4);
    sc_mailbox_t *mb = sc_mailbox_create(&a, 8);
    sc_policy_engine_t *pe = sc_policy_engine_create(&a);
    agent.agent_pool = pool;
    agent.mailbox = mb;
    agent.policy_engine = pe;
    SC_ASSERT_NOT_NULL(agent.agent_pool);
    SC_ASSERT_NOT_NULL(agent.mailbox);
    SC_ASSERT_NOT_NULL(agent.policy_engine);
    sc_policy_engine_destroy(pe);
    sc_mailbox_destroy(mb);
    sc_agent_pool_destroy(pool);
}

static void test_integ_factory_pool(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *pool = sc_agent_pool_create(&a, 2);
    sc_tool_t *tools = NULL;
    size_t count = 0;
    sc_error_t err = sc_tools_create_default(&a, ".", 1, NULL, NULL, NULL, NULL, pool, &tools, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(count > 0);
    sc_tools_destroy_default(&a, tools, count);
    sc_agent_pool_destroy(pool);
}

static void test_integ_factory_null_pool(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_tool_t *tools = NULL;
    size_t count = 0;
    sc_error_t err = sc_tools_create_default(&a, ".", 1, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    SC_ASSERT_EQ(err, SC_OK);
    sc_tools_destroy_default(&a, tools, count);
}

static void test_integ_policy_deny(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_policy_engine_t *pe = sc_policy_engine_create(&a);
    sc_policy_match_t m = { .tool = "shell" };
    sc_policy_engine_add_rule(pe, "d", m, SC_POLICY_DENY, "no");
    sc_policy_eval_ctx_t ctx = { .tool_name = "shell", .args_json = "{}" };
    sc_policy_result_t res = sc_policy_engine_evaluate(pe, &ctx);
    SC_ASSERT_EQ(res.action, SC_POLICY_DENY);
    sc_policy_engine_destroy(pe);
}

static void test_integ_policy_allow(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_policy_engine_t *pe = sc_policy_engine_create(&a);
    sc_policy_match_t m = { .tool = "shell" };
    sc_policy_engine_add_rule(pe, "d", m, SC_POLICY_DENY, "no");
    sc_policy_eval_ctx_t ctx = { .tool_name = "file_read", .args_json = "{}" };
    sc_policy_result_t res = sc_policy_engine_evaluate(pe, &ctx);
    SC_ASSERT_EQ(res.action, SC_POLICY_ALLOW);
    sc_policy_engine_destroy(pe);
}

static void test_integ_profile_coding(void) {
    const sc_agent_profile_t *p = sc_agent_profile_get(SC_PROFILE_CODING);
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT(p->max_iterations > 0);
    SC_ASSERT(p->max_history > 0);
    SC_ASSERT_NOT_NULL(p->preferred_model);
}

static void test_integ_pool_roundtrip(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *pool = sc_agent_pool_create(&a, 4);
    sc_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = SC_SPAWN_ONE_SHOT;
    cfg.max_iterations = 5;
    uint64_t id = 0;
    SC_ASSERT_EQ(sc_agent_pool_spawn(pool, &cfg, "t", 1, "l", &id), SC_OK);
    SC_ASSERT(id > 0);
    sc_agent_pool_info_t *info = NULL;
    size_t ic = 0;
    SC_ASSERT_EQ(sc_agent_pool_list(pool, &a, &info, &ic), SC_OK);
    SC_ASSERT(ic >= 1);
    if (info) a.free(a.ctx, info, ic * sizeof(sc_agent_pool_info_t));
    SC_ASSERT_EQ(sc_agent_pool_cancel(pool, id), SC_OK);
    sc_agent_pool_destroy(pool);
}

static void test_integ_mailbox_roundtrip(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *mb = sc_mailbox_create(&a, 8);
    SC_ASSERT_EQ(sc_mailbox_register(mb, 10), SC_OK);
    SC_ASSERT_EQ(sc_mailbox_register(mb, 20), SC_OK);
    SC_ASSERT_EQ(sc_mailbox_send(mb, 10, 20, SC_MSG_TASK, "hi", 2, 42), SC_OK);
    sc_message_t msg;
    SC_ASSERT_EQ(sc_mailbox_recv(mb, 20, &msg), SC_OK);
    SC_ASSERT_EQ(msg.type, SC_MSG_TASK);
    SC_ASSERT_STR_EQ(msg.payload, "hi");
    a.free(a.ctx, msg.payload, msg.payload_len + 1);
    sc_mailbox_destroy(mb);
}

static void test_integ_thread_binding(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_thread_binding_t *tb = sc_thread_binding_create(&a, 16);
    SC_ASSERT_NOT_NULL(tb);
    SC_ASSERT_EQ(sc_thread_binding_bind(tb, "discord", "thread-123", 42, 3600), SC_OK);
    uint64_t aid = 0;
    SC_ASSERT_EQ(sc_thread_binding_lookup(tb, "discord", "thread-123", &aid), SC_OK);
    SC_ASSERT_EQ(aid, 42);
    SC_ASSERT_EQ(sc_thread_binding_count(tb), 1);
    sc_thread_binding_destroy(tb);
}

static void test_integ_plugin_registry(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_plugin_registry_t *reg = sc_plugin_registry_create(&a, 8);
    SC_ASSERT_NOT_NULL(reg);
    SC_ASSERT_EQ(sc_plugin_count(reg), 0);
    sc_plugin_registry_destroy(reg);
}

'''
    c = c.replace('void run_roadmap_tests(void) {', integ + 'void run_roadmap_tests(void) {')
    c = c.replace(
        '    SC_RUN_TEST(test_plugin_bad_version);\n}',
        """    SC_RUN_TEST(test_plugin_bad_version);

    SC_TEST_SUITE("Integration: Config Wiring");
    SC_RUN_TEST(test_integ_config_defaults);
    SC_RUN_TEST(test_integ_config_parse_pool);
    SC_RUN_TEST(test_integ_config_parse_policy);

    SC_TEST_SUITE("Integration: Agent Struct");
    SC_RUN_TEST(test_integ_agent_fields);

    SC_TEST_SUITE("Integration: Factory + Pool");
    SC_RUN_TEST(test_integ_factory_pool);
    SC_RUN_TEST(test_integ_factory_null_pool);

    SC_TEST_SUITE("Integration: Policy Engine E2E");
    SC_RUN_TEST(test_integ_policy_deny);
    SC_RUN_TEST(test_integ_policy_allow);

    SC_TEST_SUITE("Integration: Profiles");
    SC_RUN_TEST(test_integ_profile_coding);

    SC_TEST_SUITE("Integration: Pool Roundtrip");
    SC_RUN_TEST(test_integ_pool_roundtrip);

    SC_TEST_SUITE("Integration: Mailbox Roundtrip");
    SC_RUN_TEST(test_integ_mailbox_roundtrip);

    SC_TEST_SUITE("Integration: Thread Binding");
    SC_RUN_TEST(test_integ_thread_binding);

    SC_TEST_SUITE("Integration: Plugin Registry");
    SC_RUN_TEST(test_integ_plugin_registry);
}""")

    with open(path, 'w') as f:
        f.write(c)

# ════════════════════════════════════════════════════════════════════
# 8. PRE-EXISTING BUG FIXES
# ════════════════════════════════════════════════════════════════════

# config.c: .buf -> .ptr
patch('src/config.c', [
    ('sc_strndup(a, item->data.string.buf,', 'sc_strndup(a, item->data.string.ptr,'),
])

# session.c: fix undeclared functions
patch('src/session.c', [
    ('sc_json_value_t *root = sc_json_parse(mgr->alloc, buf, rd);',
     'sc_json_value_t *root = NULL;\n    sc_error_t perr = sc_json_parse(mgr->alloc, buf, rd, &root);'),
    ('if (!root) { mgr->alloc->free(mgr->alloc->ctx, buf, (size_t)flen + 1); return SC_ERR_PARSE; }',
     'if (perr != SC_OK || !root) { mgr->alloc->free(mgr->alloc->ctx, buf, (size_t)flen + 1); return SC_ERR_PARSE; }'),
    ('size_t arr_len = sc_json_array_length(root);',
     'size_t arr_len = (root->type == SC_JSON_ARRAY) ? root->data.array.len : 0;'),
    ('const sc_json_value_t *item = sc_json_array_get(root, i);',
     'const sc_json_value_t *item = root->data.array.items[i];'),
    ('const sc_json_value_t *msgs = sc_json_get(item, "messages");',
     'const sc_json_value_t *msgs = sc_json_object_get(item, "messages");'),
    ('size_t mlen = sc_json_array_length(msgs);',
     'size_t mlen = (msgs->type == SC_JSON_ARRAY) ? msgs->data.array.len : 0;'),
    ('const sc_json_value_t *msg = sc_json_array_get(msgs, m);',
     'const sc_json_value_t *msg = msgs->data.array.items[m];'),
])

# test_roadmap.c: SC_MSG_TEXT -> SC_MSG_TASK (if still present)
patch('tests/test_roadmap.c', [
    ('SC_MSG_TEXT', 'SC_MSG_TASK'),
])

# ════════════════════════════════════════════════════════════════════
# VERIFY + BUILD + TEST
# ════════════════════════════════════════════════════════════════════

checks = {
    'include/seaclaw/agent.h': 'agent_pool',
    'include/seaclaw/config.h': 'pool_max_concurrent',
    'include/seaclaw/seaclaw.h': 'agent/spawn.h',
    'include/seaclaw/tools/factory.h': 'agent_pool',
    'src/agent/agent.c': '/spawn',
    'src/agent/cli.c': 'cli_agent_pool',
    'src/config.c': 'pool_max_concurrent',
    'src/tools/factory.c': 'agent_pool',
    'src/main.c': 'svc_mailbox',
    'tests/test_roadmap.c': 'test_integ_config_defaults',
}
for p, k in checks.items():
    with open(p) as f:
        assert k in f.read(), f'VERIFY FAIL: {k} not in {p}'
print('All patches verified OK')

# Build
r = subprocess.run(['cmake', '--build', '.', '-j8'], capture_output=True, text=True, cwd='build')
print(f'Build: exit={r.returncode}')
if r.returncode != 0:
    for l in (r.stdout+r.stderr).strip().split('\n')[-20:]:
        print(l)
    sys.exit(1)

# Test
r2 = subprocess.run(['./seaclaw_tests'], capture_output=True, text=True, cwd='build')
lines = r2.stdout.strip().split('\n')
for l in lines[-8:]:
    print(l)
print(f'Tests: exit={r2.returncode}')
if r2.returncode != 0:
    # Show failures
    for l in lines:
        if 'FAIL' in l:
            print(l)
