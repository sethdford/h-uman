#ifndef SC_AGENT_H
#define SC_AGENT_H

#include "seaclaw/agent/mailbox.h"
#include "seaclaw/agent/spawn.h"
#include "seaclaw/agent/team.h"
#include "seaclaw/agent/worktree.h"
#include "seaclaw/agent/task_list.h"
#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/slice.h"
#include "seaclaw/cost.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/retrieval.h"
#include "seaclaw/observer.h"
#include "seaclaw/provider.h"
#include "seaclaw/security.h"
#include "seaclaw/security/audit.h"
#include "seaclaw/security/policy_engine.h"
#include "seaclaw/tool.h"
#include "seaclaw/voice.h"
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Agent — orchestration loop, history, tool dispatch, slash commands
 * ────────────────────────────────────────────────────────────────────────── */

/* Owned chat message (content is heap-allocated, caller owns lifecycle) */
typedef struct sc_owned_message {
    sc_role_t role;
    char *content; /* owned; freed by sc_agent_clear_history */
    size_t content_len;
    char *name; /* optional, for tool results */
    size_t name_len;
    char *tool_call_id; /* optional */
    size_t tool_call_id_len;
    sc_tool_call_t *tool_calls; /* optional, owned; for assistant with tool_calls */
    size_t tool_calls_count;
} sc_owned_message_t;

typedef struct sc_agent sc_agent_t;

/* Optional context pressure config. Pass to sc_agent_from_config; NULL = use defaults. */
typedef struct sc_agent_context_config {
    uint64_t token_limit;       /* 0 = resolve from model at runtime */
    float pressure_warn;        /* warn at this ratio (default 0.85) */
    float pressure_compact;     /* auto-compact at this ratio (default 0.95) */
    float compact_target;      /* compact until below this ratio (default 0.70) */
} sc_agent_context_config_t;

/* Called when a tool needs user approval before execution.
 * tool_name/args describe the pending action.
 * Return true to approve, false to deny. */
typedef bool (*sc_agent_approval_cb)(void *ctx, const char *tool_name, const char *args);

struct sc_agent {
    sc_allocator_t *alloc;
    sc_provider_t provider;
    sc_tool_t *tools;
    size_t tools_count;
    sc_tool_spec_t *tool_specs; /* owned; built from tools */
    size_t tool_specs_count;
    sc_memory_t *memory;                     /* optional, may be NULL */
    sc_retrieval_engine_t *retrieval_engine; /* optional; when set, memory_loader uses it */
    sc_session_store_t *session_store;       /* optional, may be NULL */
    sc_observer_t *observer;                 /* optional, may be NULL */
    sc_security_policy_t *policy;            /* optional, may be NULL */
    sc_cost_tracker_t *cost_tracker;         /* optional, may be NULL */

    char *model_name; /* owned */
    size_t model_name_len;
    char *default_provider; /* owned */
    size_t default_provider_len;
    double temperature;
    char *workspace_dir; /* owned */
    size_t workspace_dir_len;
    uint32_t max_tool_iterations;
    uint32_t max_history_messages;
    bool auto_save;
    uint8_t autonomy_level;    /* 0=readonly, 1=supervised, 2=full */
    char *custom_instructions; /* optional user system instructions */
    size_t custom_instructions_len;

    sc_owned_message_t *history; /* owned array; grows */
    size_t history_count;
    size_t history_cap;
    uint64_t total_tokens;

    /* Context pressure: token_limit from config (0 = resolve from model) */
    uint64_t token_limit;
    float context_pressure_warn;
    float context_pressure_compact;
    float context_compact_target;
    bool context_pressure_warning_85_emitted;
    bool context_pressure_warning_95_emitted;

    /* Cached static portion of system prompt (rebuilt only when config changes) */
    char *cached_static_prompt;
    size_t cached_static_prompt_len;
    size_t cached_static_prompt_cap;

    volatile sig_atomic_t cancel_requested; /* set by SIGINT handler to abort turn */

    sc_agent_approval_cb approval_cb; /* optional; if NULL, approval-required = denied */
    void *approval_ctx;

    /* TTS (text-to-speech) auto-playback */
    bool tts_enabled;                /* if true, play responses as audio */
    sc_voice_config_t *voice_config; /* optional; if NULL, TTS is skipped even if enabled */

    sc_arena_t *turn_arena; /* per-turn bump allocator for ephemeral allocations */

    sc_agent_pool_t *agent_pool;
    sc_mailbox_t *mailbox;
    uint64_t agent_id; /* used for mailbox registration; 0 = use (uintptr_t)agent */
    sc_worktree_manager_t *worktree_mgr;
    sc_team_t *team;
    sc_task_list_t *task_list;
    sc_policy_engine_t *policy_engine;

    sc_audit_logger_t *audit_logger;
    struct sc_undo_stack *undo_stack;
};

/* Create agent from minimal config (no full config loader yet).
 * ctx_cfg: optional context pressure config; NULL = use defaults. */
sc_error_t sc_agent_from_config(sc_agent_t *out, sc_allocator_t *alloc, sc_provider_t provider,
                                const sc_tool_t *tools, size_t tools_count, sc_memory_t *memory,
                                sc_session_store_t *session_store, sc_observer_t *observer,
                                sc_security_policy_t *policy, const char *model_name,
                                size_t model_name_len, const char *default_provider,
                                size_t default_provider_len, double temperature,
                                const char *workspace_dir, size_t workspace_dir_len,
                                uint32_t max_tool_iterations, uint32_t max_history_messages,
                                bool auto_save, uint8_t autonomy_level,
                                const char *custom_instructions, size_t custom_instructions_len,
                                const sc_agent_context_config_t *ctx_cfg);

void sc_agent_deinit(sc_agent_t *agent);

/* Optional: set mailbox and register agent for multi-agent messaging. Caller owns mailbox. */
void sc_agent_set_mailbox(sc_agent_t *agent, sc_mailbox_t *mailbox);

/* Optional: set shared task list for multi-agent collaboration. Caller owns task_list. */
void sc_agent_set_task_list(sc_agent_t *agent, sc_task_list_t *task_list);

/* Optional: set retrieval engine for semantic/hybrid recall. Caller owns engine lifecycle. */
void sc_agent_set_retrieval_engine(sc_agent_t *agent, sc_retrieval_engine_t *engine);

/* Run one conversation turn: send to provider, process tool calls, iterate. */
sc_error_t sc_agent_turn(sc_agent_t *agent, const char *msg, size_t msg_len, char **response_out,
                         size_t *response_len_out);

/* Optional: if non-NULL, called for each streaming token delta (CLI mode).
 * Provider must support streaming. When provided, uses stream_chat when available. */
typedef void (*sc_agent_stream_token_cb)(const char *delta, size_t len, void *ctx);

sc_error_t sc_agent_turn_stream(sc_agent_t *agent, const char *msg, size_t msg_len,
                                sc_agent_stream_token_cb on_token, void *token_ctx,
                                char **response_out, size_t *response_len_out);

/* Run a single message without history (no tool loop for simplicity in Phase 4). */
sc_error_t sc_agent_run_single(sc_agent_t *agent, const char *system_prompt,
                               size_t system_prompt_len, const char *user_message,
                               size_t user_message_len, char **response_out,
                               size_t *response_len_out);

void sc_agent_clear_history(sc_agent_t *agent);

/* Handle slash commands: /help, /quit, /clear, /model, /status.
 * Returns owned response string or NULL if not a slash command.
 * Caller must free the returned string. */
char *sc_agent_handle_slash_command(sc_agent_t *agent, const char *message, size_t message_len);

/* Estimate tokens for a string (rough: ~4 chars per token). */
uint32_t sc_agent_estimate_tokens(const char *text, size_t len);

/* Execute a structured plan (Tier 1.4 planner integration).
 * plan_json format: {"steps": [{"tool": "name", "args": {...}, "description": "..."}]}
 * Returns a summary of execution results. Caller must free summary_out. */
sc_error_t sc_agent_execute_plan(sc_agent_t *agent, const char *plan_json, size_t plan_json_len,
                                 char **summary_out, size_t *summary_len_out);

#endif /* SC_AGENT_H */
