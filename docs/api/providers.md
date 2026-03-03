---
title: Provider API
description: sc_provider_t vtable for OpenAI, Anthropic, Gemini, Ollama, and other LLM backends
updated: 2026-03-02
---

# Provider API

Providers implement the `sc_provider_t` vtable to connect to LLM backends (OpenAI, Anthropic, Gemini, Ollama, etc.).

## Types

### Chat Types

```c
typedef enum sc_role { SC_ROLE_SYSTEM, SC_ROLE_USER, SC_ROLE_ASSISTANT, SC_ROLE_TOOL } sc_role_t;

typedef struct sc_chat_message {
    sc_role_t role;
    const char *content;
    size_t content_len;
    const char *name;
    size_t name_len;
    const char *tool_call_id;
    size_t tool_call_id_len;
    const sc_content_part_t *content_parts;
    size_t content_parts_count;
    const sc_tool_call_t *tool_calls;
    size_t tool_calls_count;
} sc_chat_message_t;

typedef struct sc_tool_call {
    const char *id;
    size_t id_len;
    const char *name;
    size_t name_len;
    const char *arguments;
    size_t arguments_len;
} sc_tool_call_t;

typedef struct sc_chat_request {
    const sc_chat_message_t *messages;
    size_t messages_count;
    const char *model;
    size_t model_len;
    double temperature;
    uint32_t max_tokens;
    const sc_tool_spec_t *tools;
    size_t tools_count;
    uint64_t timeout_secs;
    const char *reasoning_effort;
    size_t reasoning_effort_len;
} sc_chat_request_t;

typedef struct sc_chat_response {
    const char *content;
    size_t content_len;
    const sc_tool_call_t *tool_calls;
    size_t tool_calls_count;
    sc_token_usage_t usage;
    const char *model;
    size_t model_len;
    const char *reasoning_content;
    size_t reasoning_content_len;
} sc_chat_response_t;
```

### Provider Vtable

```c
typedef struct sc_provider {
    void *ctx;
    const struct sc_provider_vtable *vtable;
} sc_provider_t;

typedef struct sc_provider_vtable {
    /* Required */
    sc_error_t (*chat_with_system)(void *ctx, sc_allocator_t *alloc,
        const char *system_prompt, size_t system_prompt_len,
        const char *message, size_t message_len,
        const char *model, size_t model_len,
        double temperature,
        char **out, size_t *out_len);

    sc_error_t (*chat)(void *ctx, sc_allocator_t *alloc,
        const sc_chat_request_t *request,
        const char *model, size_t model_len,
        double temperature,
        sc_chat_response_t *out);

    bool (*supports_native_tools)(void *ctx);
    const char *(*get_name)(void *ctx);
    void (*deinit)(void *ctx, sc_allocator_t *alloc);

    /* Optional — may be NULL */
    void (*warmup)(void *ctx);
    sc_error_t (*chat_with_tools)(void *ctx, sc_allocator_t *alloc,
        const sc_chat_request_t *req, sc_chat_response_t *out);
    bool (*supports_streaming)(void *ctx);
    bool (*supports_vision)(void *ctx);
    sc_error_t (*stream_chat)(void *ctx, sc_allocator_t *alloc,
        const sc_chat_request_t *request,
        const char *model, size_t model_len,
        double temperature,
        sc_stream_callback_t callback, void *callback_ctx,
        sc_stream_chat_result_t *out);
} sc_provider_vtable_t;
```

## Factory

```c
sc_error_t sc_provider_create(sc_allocator_t *alloc,
    const char *name, size_t name_len,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out);
```

**Names:** `openai`, `anthropic`, `gemini`, `ollama`, `openrouter`, `compatible`, `claude_cli`, `codex_cli`, `openai_codex`. Compatible providers (e.g. `groq`, `mistral`) use the compatible backend with a known base URL.

## Helper

```c
void sc_chat_response_free(sc_allocator_t *alloc, sc_chat_response_t *resp);
const char *sc_compatible_provider_url(const char *name);
```

## Usage Example

```c
sc_allocator_t alloc = sc_system_allocator();
sc_provider_t prov;
sc_error_t err = sc_provider_create(&alloc, "openai", 6,
    getenv("OPENAI_API_KEY"), strlen(getenv("OPENAI_API_KEY")),
    NULL, 0, &prov);
if (err != SC_OK) { /* handle */ }

sc_chat_request_t req = {
    .messages = msgs,
    .messages_count = 2,
    .model = "gpt-4o-mini",
    .model_len = 10,
    .temperature = 0.7,
    .max_tokens = 1024,
    .tools = NULL,
    .tools_count = 0,
    .timeout_secs = 60,
};
sc_chat_response_t resp = {0};
err = prov.vtable->chat(prov.ctx, &alloc, &req, req.model, req.model_len, req.temperature, &resp);
if (err == SC_OK && resp.content) {
    printf("%.*s\n", (int)resp.content_len, resp.content);
    sc_chat_response_free(&alloc, &resp);
}
prov.vtable->deinit(prov.ctx, &alloc);
```

## Allocation Requirements

- `chat` / `chat_with_tools` allocate `content`, `model`, and tool call strings. Use `sc_chat_response_free` to release.
- Caller owns the provider struct; `deinit` frees provider-internal allocations.
