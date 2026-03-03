---
title: Core API
description: Allocator, error, JSON, string, slice, HTTP, and arena utilities
updated: 2026-03-02
---

# Core API

Core types and utilities used across seaclaw: allocator, error, JSON, string, slice, HTTP, arena.

## Allocator (`core/allocator.h`)

```c
typedef struct sc_allocator {
    void *ctx;
    void* (*alloc)(void *ctx, size_t size);
    void* (*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void  (*free)(void *ctx, void *ptr, size_t size);
} sc_allocator_t;

sc_allocator_t sc_system_allocator(void);
```

**Usage:** Obtain system allocator, pass to API functions that need heap allocation.

```c
sc_allocator_t alloc = sc_system_allocator();
char *buf = alloc.alloc(alloc.ctx, 1024);
if (buf) {
    /* use buf */
    alloc.free(alloc.ctx, buf, 1024);
}
```

### Tracking Allocator (tests/debugging)

```c
sc_tracking_allocator_t *ta = sc_tracking_allocator_create();
sc_allocator_t alloc = sc_tracking_allocator_allocator(ta);
/* ... use alloc ... */
size_t leaks = sc_tracking_allocator_leaks(ta);
sc_tracking_allocator_destroy(ta);
```

---

## Error (`core/error.h`)

```c
typedef enum sc_error {
    SC_OK = 0,
    SC_ERR_OUT_OF_MEMORY,
    SC_ERR_INVALID_ARGUMENT,
    SC_ERR_NOT_FOUND,
    /* ... (see header for full list) */
} sc_error_t;

const char *sc_error_string(sc_error_t err);
```

---

## Slice / String (`core/slice.h`)

```c
typedef struct sc_str {
    const char *ptr;
    size_t len;
} sc_str_t;

typedef struct sc_bytes {
    const uint8_t *ptr;
    size_t len;
} sc_bytes_t;

#define SC_STR_LIT(s) ((sc_str_t){ .ptr = (s), .len = sizeof(s) - 1 })
#define SC_STR_NULL   ((sc_str_t){ .ptr = NULL, .len = 0 })
```

**Inline helpers:** `sc_str_from_cstr`, `sc_str_is_empty`, `sc_str_eq`, `sc_str_starts_with`, `sc_str_ends_with`, `sc_str_trim`.

---

## String (`core/string.h`)

```c
char *sc_strdup(sc_allocator_t *alloc, const char *s);
char *sc_strndup(sc_allocator_t *alloc, const char *s, size_t n);
char *sc_str_dup(sc_allocator_t *alloc, sc_str_t s);
char *sc_str_concat(sc_allocator_t *alloc, sc_str_t a, sc_str_t b);
char *sc_str_join(sc_allocator_t *alloc, const sc_str_t *parts, size_t count, sc_str_t sep);
char *sc_sprintf(sc_allocator_t *alloc, const char *fmt, ...);
void sc_str_free(sc_allocator_t *alloc, char *s);
bool sc_str_contains(sc_str_t haystack, sc_str_t needle);
int sc_str_index_of(sc_str_t haystack, sc_str_t needle);
```

---

## JSON (`core/json.h`)

```c
typedef enum sc_json_type {
    SC_JSON_NULL, SC_JSON_BOOL, SC_JSON_NUMBER,
    SC_JSON_STRING, SC_JSON_ARRAY, SC_JSON_OBJECT
} sc_json_type_t;

typedef struct sc_json_value sc_json_value_t;

sc_error_t sc_json_parse(sc_allocator_t *alloc, const char *input, size_t input_len, sc_json_value_t **out);
void sc_json_free(sc_allocator_t *alloc, sc_json_value_t *val);

sc_json_value_t *sc_json_null_new(sc_allocator_t *alloc);
sc_json_value_t *sc_json_bool_new(sc_allocator_t *alloc, bool val);
sc_json_value_t *sc_json_number_new(sc_allocator_t *alloc, double val);
sc_json_value_t *sc_json_string_new(sc_allocator_t *alloc, const char *s, size_t len);
sc_json_value_t *sc_json_array_new(sc_allocator_t *alloc);
sc_json_value_t *sc_json_object_new(sc_allocator_t *alloc);

sc_error_t sc_json_array_push(sc_allocator_t *alloc, sc_json_value_t *arr, sc_json_value_t *val);
sc_error_t sc_json_object_set(sc_allocator_t *alloc, sc_json_value_t *obj, const char *key, sc_json_value_t *val);
sc_json_value_t *sc_json_object_get(const sc_json_value_t *obj, const char *key);

const char *sc_json_get_string(const sc_json_value_t *val, const char *key);
double sc_json_get_number(const sc_json_value_t *val, const char *key, double default_val);
bool sc_json_get_bool(const sc_json_value_t *val, const char *key, bool default_val);

sc_error_t sc_json_stringify(sc_allocator_t *alloc, const sc_json_value_t *val, char **out, size_t *out_len);
```

**Example:**

```c
sc_json_value_t *obj = sc_json_object_new(alloc);
sc_json_object_set(alloc, obj, "name", sc_json_string_new(alloc, "test", 4));
const char *name = sc_json_get_string(obj, "name");  /* "test" */
sc_json_free(alloc, obj);
```

---

## HTTP (`core/http.h`)

Requires `SC_ENABLE_CURL=ON`.

```c
typedef struct sc_http_response {
    char *body;
    size_t body_len;
    size_t body_cap;
    long status_code;
    bool owned;
} sc_http_response_t;

sc_error_t sc_http_post_json(sc_allocator_t *alloc, const char *url,
    const char *auth_header, const char *json_body, size_t json_body_len,
    sc_http_response_t *out);

sc_error_t sc_http_post_json_ex(sc_allocator_t *alloc, const char *url,
    const char *auth_header, const char *extra_headers,
    const char *json_body, size_t json_body_len, sc_http_response_t *out);

sc_error_t sc_http_get(sc_allocator_t *alloc, const char *url,
    const char *auth_header, sc_http_response_t *out);

void sc_http_response_free(sc_allocator_t *alloc, sc_http_response_t *resp);
```

---

## Arena (`core/arena.h`)

Bump allocator for short-lived allocations.

```c
sc_arena_t *sc_arena_create(sc_allocator_t backing);
sc_allocator_t sc_arena_allocator(sc_arena_t *arena);
void sc_arena_reset(sc_arena_t *arena);
void sc_arena_destroy(sc_arena_t *arena);
size_t sc_arena_bytes_used(const sc_arena_t *arena);
```

---

## Cross-References

- **Agent** uses allocator, arena (per-turn), error, JSON.
- **Providers** use allocator, error, HTTP, JSON.
- **Tools** use allocator, error, JSON.
- **Memory** uses allocator, error.
- **Config** uses allocator, arena, error.
