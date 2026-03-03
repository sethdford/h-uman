---
title: Agent API
description: Agent module for conversation loop, tool dispatch, and slash commands
updated: 2026-03-02
---

# Agent API

The agent orchestrates the conversation loop: provider chat, tool dispatch, history, and slash commands.

## Types

```c
typedef struct sc_owned_message {
    sc_role_t role;
    char *content;
    size_t content_len;
    char *name;
    size_t name_len;
    char *tool_call_id;
    size_t tool_call_id_len;
    sc_tool_call_t *tool_calls;
    size_t tool_calls_count;
} sc_owned_message_t;

typedef struct sc_agent sc_agent_t;  /* opaquely referenced in agent.h; fields in include */
```

Key agent fields (see `agent.h`): `provider`, `tools`, `tools_count`, `memory`, `retrieval_engine`, `session_store`, `observer`, `policy`, `model_name`, `history`, `turn_arena`.

## Key Functions

### Creation

```c
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
```

### Retrieval Engine (Optional)

```c
void sc_agent_set_retrieval_engine(sc_agent_t *agent, sc_retrieval_engine_t *engine);
```

### Turn Execution

```c
sc_error_t sc_agent_turn(sc_agent_t *agent, const char *msg, size_t msg_len,
    char **response_out, size_t *response_len_out);
```

Runs one turn: appends user message, calls provider (with tool loop), returns final response. Caller must free `*response_out`.

### Streaming

```c
typedef void (*sc_agent_stream_token_cb)(const char *delta, size_t len, void *ctx);

sc_error_t sc_agent_turn_stream(sc_agent_t *agent, const char *msg, size_t msg_len,
    sc_agent_stream_token_cb on_token, void *token_ctx,
    char **response_out, size_t *response_len_out);
```

### Single Message (No History)

```c
sc_error_t sc_agent_run_single(sc_agent_t *agent,
    const char *system_prompt, size_t system_prompt_len,
    const char *user_message, size_t user_message_len,
    char **response_out, size_t *response_len_out);
```

### Slash Commands

```c
char *sc_agent_handle_slash_command(sc_agent_t *agent,
    const char *message, size_t message_len);
```

Handles `/help`, `/quit`, `/clear`, `/model`, `/status`. Returns owned string or NULL if not a slash command.

### Utility

```c
void sc_agent_clear_history(sc_agent_t *agent);
uint32_t sc_agent_estimate_tokens(const char *text, size_t len);
```

### Plan Execution

```c
sc_error_t sc_agent_execute_plan(sc_agent_t *agent, const char *plan_json, size_t plan_json_len,
    char **summary_out, size_t *summary_len_out);
```

## Usage Example

```c
sc_allocator_t alloc = sc_system_allocator();
sc_provider_t provider;
sc_provider_create(&alloc, "openai", 6, api_key, strlen(api_key), NULL, 0, &provider);

sc_tool_t tools[1];
sc_web_fetch_create(&alloc, 50000, &tools[0]);

sc_agent_t agent;
sc_agent_from_config(&agent, &alloc, provider, tools, 1, NULL, NULL, NULL, NULL,
    "gpt-4o-mini", 10, "openai", 6, 0.7,
    "/tmp/workspace", 14, 10, 50, false, SC_AUTONOMY_SUPERVISED,
    NULL, 0);

char *response;
size_t len;
sc_agent_turn(&agent, "What is 2+2?", 12, &response, &len);
printf("%.*s\n", (int)len, response);
alloc.free(alloc.ctx, response, len + 1);

sc_agent_deinit(&agent);
sc_tools_destroy_default(&alloc, tools, 1);
provider.vtable->deinit(provider.ctx, &alloc);
```

## Thread Safety

- Not thread-safe. One turn at a time per agent instance.
- `cancel_requested` is `volatile sig_atomic_t` for SIGINT handling.

## Allocation Requirements

- `sc_agent_turn` and `sc_agent_turn_stream` allocate `*response_out`; caller frees.
- `sc_agent_handle_slash_command` returns allocated string; caller frees.
- History and turn arena are managed internally.
