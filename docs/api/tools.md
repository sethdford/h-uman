---
title: Tools API
description: sc_tool_t vtable for file, web, shell, memory, and other tools
updated: 2026-03-02
---

# Tools API

Tools implement the `sc_tool_t` vtable to perform actions (file read/write, web fetch, shell, memory, etc.).

## Types

### Tool Result

```c
typedef struct sc_tool_result {
    bool success;
    const char *output;
    size_t output_len;
    const char *error_msg;
    size_t error_msg_len;
    bool output_owned;
    bool error_msg_owned;
    bool needs_approval;
} sc_tool_result_t;

/* Helpers */
sc_tool_result_t sc_tool_result_ok(const char *output, size_t len);
sc_tool_result_t sc_tool_result_ok_owned(const char *output, size_t len);
sc_tool_result_t sc_tool_result_fail(const char *error_msg, size_t len);
sc_tool_result_t sc_tool_result_fail_owned(const char *error_msg, size_t len);

void sc_tool_result_free(sc_allocator_t *alloc, sc_tool_result_t *r);
```

### Tool Vtable

```c
typedef struct sc_tool {
    void *ctx;
    const struct sc_tool_vtable *vtable;
} sc_tool_t;

typedef struct sc_tool_vtable {
    sc_error_t (*execute)(void *ctx, sc_allocator_t *alloc,
        const sc_json_value_t *args,
        sc_tool_result_t *out);
    const char *(*name)(void *ctx);
    const char *(*description)(void *ctx);
    const char *(*parameters_json)(void *ctx);
    void (*deinit)(void *ctx, sc_allocator_t *alloc);  /* optional */
} sc_tool_vtable_t;
```

### Tool Spec (for provider)

```c
typedef struct sc_tool_spec {
    const char *name;
    size_t name_len;
    const char *description;
    size_t description_len;
    const char *parameters_json;
    size_t parameters_json_len;
} sc_tool_spec_t;
```

## Factory

```c
sc_error_t sc_tools_create_default(sc_allocator_t *alloc,
    const char *workspace_dir, size_t workspace_dir_len,
    sc_security_policy_t *policy,
    const sc_config_t *config,
    sc_memory_t *memory,
    sc_cron_scheduler_t *cron,
    sc_tool_t **out_tools, size_t *out_count);

void sc_tools_destroy_default(sc_allocator_t *alloc, sc_tool_t *tools, size_t count);
```

## SC_TOOL_IMPL Macro

```c
#define SC_TOOL_IMPL(Prefix, execute_fn, name_fn, desc_fn, params_fn, deinit_fn)
```

## Usage Example

```c
sc_allocator_t alloc = sc_system_allocator();
sc_tool_t tool;
sc_web_fetch_create(&alloc, 50000, &tool);

sc_json_value_t *args = sc_json_object_new(&alloc);
sc_json_object_set(&alloc, args, "url", sc_json_string_new(&alloc, "https://example.com", 18));

sc_tool_result_t result = {0};
sc_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
if (err == SC_OK && result.success) {
    printf("%.*s\n", (int)result.output_len, result.output);
    sc_tool_result_free(&alloc, &result);
}
sc_json_free(&alloc, args);

tool.vtable->deinit(tool.ctx, &alloc);
```

## Built-in Tools (Selection)

- `file_read`, `file_write`, `file_edit`, `file_append`
- `shell`, `spawn`
- `web_fetch`, `web_search`, `http_request`
- `memory_store`, `memory_recall`, `memory_list`, `memory_forget`
- `git`, `browser`, `browser_open`, `screenshot`, `image`
- `cron_add`, `cron_list`, `cron_remove`, `cron_run`, etc.

## SC_IS_TEST

Use `#if SC_IS_TEST` to bypass side effects (network, process spawn, browser) in tests. Return stub results instead.
