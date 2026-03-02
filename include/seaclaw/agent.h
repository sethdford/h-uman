#ifndef SC_AGENT_H
#define SC_AGENT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/slice.h"
#include "seaclaw/provider.h"
#include "seaclaw/channel.h"
#include "seaclaw/tool.h"
#include "seaclaw/memory.h"
#include "seaclaw/observer.h"
#include "seaclaw/security.h"
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
    char *content;         /* owned; freed by sc_agent_clear_history */
    size_t content_len;
    char *name;            /* optional, for tool results */
    size_t name_len;
    char *tool_call_id;    /* optional */
    size_t tool_call_id_len;
    sc_tool_call_t *tool_calls; /* optional, owned; for assistant with tool_calls */
    size_t tool_calls_count;
} sc_owned_message_t;

typedef struct sc_agent sc_agent_t;

struct sc_agent {
    sc_allocator_t *alloc;
    sc_provider_t provider;
    sc_tool_t *tools;
    size_t tools_count;
    sc_tool_spec_t *tool_specs;   /* owned; built from tools */
    size_t tool_specs_count;
    sc_memory_t *memory;           /* optional, may be NULL */
    sc_session_store_t *session_store;  /* optional, may be NULL */
    sc_observer_t *observer;       /* optional, may be NULL */
    sc_security_policy_t *policy;  /* optional, may be NULL */

    char *model_name;             /* owned */
    size_t model_name_len;
    char *default_provider;        /* owned */
    size_t default_provider_len;
    double temperature;
    char *workspace_dir;           /* owned */
    size_t workspace_dir_len;
    uint32_t max_tool_iterations;
    uint32_t max_history_messages;
    bool auto_save;
    uint8_t autonomy_level;       /* 0=readonly, 1=supervised, 2=full */
    char *custom_instructions;    /* optional user system instructions */
    size_t custom_instructions_len;

    sc_owned_message_t *history;  /* owned array; grows */
    size_t history_count;
    size_t history_cap;
    uint64_t total_tokens;

    /* Cached static portion of system prompt (rebuilt only when config changes) */
    char *cached_static_prompt;
    size_t cached_static_prompt_len;
    size_t cached_static_prompt_cap;

    volatile sig_atomic_t cancel_requested;  /* set by SIGINT handler to abort turn */
};

/* Create agent from minimal config (no full config loader yet). */
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
    const char *custom_instructions, size_t custom_instructions_len);

void sc_agent_deinit(sc_agent_t *agent);

/* Run one conversation turn: send to provider, process tool calls, iterate. */
sc_error_t sc_agent_turn(sc_agent_t *agent, const char *msg, size_t msg_len,
    char **response_out, size_t *response_len_out);

/* Optional: if non-NULL, called for each streaming token delta (CLI mode).
 * Provider must support streaming. When provided, uses stream_chat when available. */
typedef void (*sc_agent_stream_token_cb)(const char *delta, size_t len, void *ctx);

sc_error_t sc_agent_turn_stream(sc_agent_t *agent, const char *msg, size_t msg_len,
    sc_agent_stream_token_cb on_token, void *token_ctx,
    char **response_out, size_t *response_len_out);

/* Run a single message without history (no tool loop for simplicity in Phase 4). */
sc_error_t sc_agent_run_single(sc_agent_t *agent,
    const char *system_prompt, size_t system_prompt_len,
    const char *user_message, size_t user_message_len,
    char **response_out, size_t *response_len_out);

void sc_agent_clear_history(sc_agent_t *agent);

/* Handle slash commands: /help, /quit, /clear, /model, /status.
 * Returns owned response string or NULL if not a slash command.
 * Caller must free the returned string. */
char *sc_agent_handle_slash_command(sc_agent_t *agent,
    const char *message, size_t message_len);

/* Estimate tokens for a string (rough: ~4 chars per token). */
uint32_t sc_agent_estimate_tokens(const char *text, size_t len);

/* Execute a structured plan (Tier 1.4 planner integration).
 * plan_json format: {"steps": [{"tool": "name", "args": {...}, "description": "..."}]}
 * Returns a summary of execution results. Caller must free summary_out. */
sc_error_t sc_agent_execute_plan(sc_agent_t *agent, const char *plan_json, size_t plan_json_len,
    char **summary_out, size_t *summary_len_out);

#endif /* SC_AGENT_H */
