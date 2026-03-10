---
title: Tools API
description: hu_tool_t vtable for file, web, shell, memory, and other tools
updated: 2026-03-02
---

# Tools API

Tools implement the `hu_tool_t` vtable to perform actions (file read/write, web fetch, shell, memory, etc.).

## Types

### Tool Result

```c
typedef struct hu_tool_result {
    bool success;
    const char *output;
    size_t output_len;
    const char *error_msg;
    size_t error_msg_len;
    bool output_owned;
    bool error_msg_owned;
    bool needs_approval;
} hu_tool_result_t;

/* Helpers */
hu_tool_result_t hu_tool_result_ok(const char *output, size_t len);
hu_tool_result_t hu_tool_result_ok_owned(const char *output, size_t len);
hu_tool_result_t hu_tool_result_fail(const char *error_msg, size_t len);
hu_tool_result_t hu_tool_result_fail_owned(const char *error_msg, size_t len);

void hu_tool_result_free(hu_allocator_t *alloc, hu_tool_result_t *r);
```

### Tool Vtable

```c
typedef struct hu_tool {
    void *ctx;
    const struct hu_tool_vtable *vtable;
} hu_tool_t;

typedef struct hu_tool_vtable {
    hu_error_t (*execute)(void *ctx, hu_allocator_t *alloc,
        const hu_json_value_t *args,
        hu_tool_result_t *out);
    const char *(*name)(void *ctx);
    const char *(*description)(void *ctx);
    const char *(*parameters_json)(void *ctx);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);  /* optional */
} hu_tool_vtable_t;
```

### Tool Spec (for provider)

```c
typedef struct hu_tool_spec {
    const char *name;
    size_t name_len;
    const char *description;
    size_t description_len;
    const char *parameters_json;
    size_t parameters_json_len;
} hu_tool_spec_t;
```

## Factory

```c
hu_error_t hu_tools_create_default(hu_allocator_t *alloc,
    const char *workspace_dir, size_t workspace_dir_len,
    hu_security_policy_t *policy,
    const hu_config_t *config,
    hu_memory_t *memory,
    hu_cron_scheduler_t *cron,
    hu_tool_t **out_tools, size_t *out_count);

void hu_tools_destroy_default(hu_allocator_t *alloc, hu_tool_t *tools, size_t count);
```

## HU_TOOL_IMPL Macro

```c
#define HU_TOOL_IMPL(Prefix, execute_fn, name_fn, desc_fn, params_fn, deinit_fn)
```

## Usage Example

```c
hu_allocator_t alloc = hu_system_allocator();
hu_tool_t tool;
hu_web_fetch_create(&alloc, 50000, &tool);

hu_json_value_t *args = hu_json_object_new(&alloc);
hu_json_object_set(&alloc, args, "url", hu_json_string_new(&alloc, "https://example.com", 18));

hu_tool_result_t result = {0};
hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
if (err == HU_OK && result.success) {
    printf("%.*s\n", (int)result.output_len, result.output);
    hu_tool_result_free(&alloc, &result);
}
hu_json_free(&alloc, args);

tool.vtable->deinit(tool.ctx, &alloc);
```

## Built-in Tools (Selection)

- `file_read`, `file_write`, `file_edit`, `file_append`
- `shell`, `spawn`
- `web_fetch`, `web_search`, `http_request`
- `memory_store`, `memory_recall`, `memory_list`, `memory_forget`
- `git`, `browser`, `browser_open`, `screenshot`, `image`
- `cron_add`, `cron_list`, `cron_remove`, `cron_run`, etc.

## HU_IS_TEST

Use `#if HU_IS_TEST` to bypass side effects (network, process spawn, browser) in tests. Return stub results instead.
