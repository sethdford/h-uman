---
title: Core API
description: Allocator, error, JSON, string, slice, HTTP, and arena utilities
updated: 2026-03-02
---

# Core API

Core types and utilities used across human: allocator, error, JSON, string, slice, HTTP, arena.

## Allocator (`core/allocator.h`)

```c
typedef struct hu_allocator {
    void *ctx;
    void* (*alloc)(void *ctx, size_t size);
    void* (*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void  (*free)(void *ctx, void *ptr, size_t size);
} hu_allocator_t;

hu_allocator_t hu_system_allocator(void);
```

**Usage:** Obtain system allocator, pass to API functions that need heap allocation.

```c
hu_allocator_t alloc = hu_system_allocator();
char *buf = alloc.alloc(alloc.ctx, 1024);
if (buf) {
    /* use buf */
    alloc.free(alloc.ctx, buf, 1024);
}
```

### Tracking Allocator (tests/debugging)

```c
hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
/* ... use alloc ... */
size_t leaks = hu_tracking_allocator_leaks(ta);
hu_tracking_allocator_destroy(ta);
```

---

## Error (`core/error.h`)

```c
typedef enum hu_error {
    HU_OK = 0,
    HU_ERR_OUT_OF_MEMORY,
    HU_ERR_INVALID_ARGUMENT,
    HU_ERR_NOT_FOUND,
    /* ... (see header for full list) */
} hu_error_t;

const char *hu_error_string(hu_error_t err);
```

---

## Slice / String (`core/slice.h`)

```c
typedef struct hu_str {
    const char *ptr;
    size_t len;
} hu_str_t;

typedef struct hu_bytes {
    const uint8_t *ptr;
    size_t len;
} hu_bytes_t;

#define HU_STR_LIT(s) ((hu_str_t){ .ptr = (s), .len = sizeof(s) - 1 })
#define HU_STR_NULL   ((hu_str_t){ .ptr = NULL, .len = 0 })
```

**Inline helpers:** `hu_str_from_cstr`, `hu_str_is_empty`, `hu_str_eq`, `hu_str_starts_with`, `hu_str_ends_with`, `hu_str_trim`.

---

## String (`core/string.h`)

```c
char *hu_strdup(hu_allocator_t *alloc, const char *s);
char *hu_strndup(hu_allocator_t *alloc, const char *s, size_t n);
char *hu_str_dup(hu_allocator_t *alloc, hu_str_t s);
char *hu_str_concat(hu_allocator_t *alloc, hu_str_t a, hu_str_t b);
char *hu_str_join(hu_allocator_t *alloc, const hu_str_t *parts, size_t count, hu_str_t sep);
char *hu_sprintf(hu_allocator_t *alloc, const char *fmt, ...);
void hu_str_free(hu_allocator_t *alloc, char *s);
bool hu_str_contains(hu_str_t haystack, hu_str_t needle);
int hu_str_index_of(hu_str_t haystack, hu_str_t needle);
```

---

## JSON (`core/json.h`)

```c
typedef enum hu_json_type {
    HU_JSON_NULL, HU_JSON_BOOL, HU_JSON_NUMBER,
    HU_JSON_STRING, HU_JSON_ARRAY, HU_JSON_OBJECT
} hu_json_type_t;

typedef struct hu_json_value hu_json_value_t;

hu_error_t hu_json_parse(hu_allocator_t *alloc, const char *input, size_t input_len, hu_json_value_t **out);
void hu_json_free(hu_allocator_t *alloc, hu_json_value_t *val);

hu_json_value_t *hu_json_null_new(hu_allocator_t *alloc);
hu_json_value_t *hu_json_bool_new(hu_allocator_t *alloc, bool val);
hu_json_value_t *hu_json_number_new(hu_allocator_t *alloc, double val);
hu_json_value_t *hu_json_string_new(hu_allocator_t *alloc, const char *s, size_t len);
hu_json_value_t *hu_json_array_new(hu_allocator_t *alloc);
hu_json_value_t *hu_json_object_new(hu_allocator_t *alloc);

hu_error_t hu_json_array_push(hu_allocator_t *alloc, hu_json_value_t *arr, hu_json_value_t *val);
hu_error_t hu_json_object_set(hu_allocator_t *alloc, hu_json_value_t *obj, const char *key, hu_json_value_t *val);
hu_json_value_t *hu_json_object_get(const hu_json_value_t *obj, const char *key);

const char *hu_json_get_string(const hu_json_value_t *val, const char *key);
double hu_json_get_number(const hu_json_value_t *val, const char *key, double default_val);
bool hu_json_get_bool(const hu_json_value_t *val, const char *key, bool default_val);

hu_error_t hu_json_stringify(hu_allocator_t *alloc, const hu_json_value_t *val, char **out, size_t *out_len);
```

**Example:**

```c
hu_json_value_t *obj = hu_json_object_new(alloc);
hu_json_object_set(alloc, obj, "name", hu_json_string_new(alloc, "test", 4));
const char *name = hu_json_get_string(obj, "name");  /* "test" */
hu_json_free(alloc, obj);
```

---

## HTTP (`core/http.h`)

Requires `HU_ENABLE_CURL=ON`.

```c
typedef struct hu_http_response {
    char *body;
    size_t body_len;
    size_t body_cap;
    long status_code;
    bool owned;
} hu_http_response_t;

hu_error_t hu_http_post_json(hu_allocator_t *alloc, const char *url,
    const char *auth_header, const char *json_body, size_t json_body_len,
    hu_http_response_t *out);

hu_error_t hu_http_post_json_ex(hu_allocator_t *alloc, const char *url,
    const char *auth_header, const char *extra_headers,
    const char *json_body, size_t json_body_len, hu_http_response_t *out);

hu_error_t hu_http_get(hu_allocator_t *alloc, const char *url,
    const char *auth_header, hu_http_response_t *out);

void hu_http_response_free(hu_allocator_t *alloc, hu_http_response_t *resp);
```

---

## Arena (`core/arena.h`)

Bump allocator for short-lived allocations.

```c
hu_arena_t *hu_arena_create(hu_allocator_t backing);
hu_allocator_t hu_arena_allocator(hu_arena_t *arena);
void hu_arena_reset(hu_arena_t *arena);
void hu_arena_destroy(hu_arena_t *arena);
size_t hu_arena_bytes_used(const hu_arena_t *arena);
```

---

## Cross-References

- **Agent** uses allocator, arena (per-turn), error, JSON.
- **Providers** use allocator, error, HTTP, JSON.
- **Tools** use allocator, error, JSON.
- **Memory** uses allocator, error.
- **Config** uses allocator, arena, error.
