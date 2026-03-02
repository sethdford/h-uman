#ifndef SC_JSON_H
#define SC_JSON_H

#include "allocator.h"
#include "slice.h"
#include "error.h"
#include <stdbool.h>

typedef enum sc_json_type {
    SC_JSON_NULL,
    SC_JSON_BOOL,
    SC_JSON_NUMBER,
    SC_JSON_STRING,
    SC_JSON_ARRAY,
    SC_JSON_OBJECT
} sc_json_type_t;

typedef struct sc_json_value sc_json_value_t;
typedef struct sc_json_pair sc_json_pair_t;

struct sc_json_pair {
    char *key;
    size_t key_len;
    sc_json_value_t *value;
};

struct sc_json_value {
    sc_json_type_t type;
    union {
        bool boolean;
        double number;
        struct { char *ptr; size_t len; } string;
        struct { sc_json_value_t **items; size_t len; size_t cap; } array;
        struct { sc_json_pair_t *pairs; size_t len; size_t cap; } object;
    } data;
};

sc_error_t sc_json_parse(sc_allocator_t *alloc, const char *input, size_t input_len,
                         sc_json_value_t **out);
void sc_json_free(sc_allocator_t *alloc, sc_json_value_t *val);

sc_json_value_t *sc_json_null_new(sc_allocator_t *alloc);
sc_json_value_t *sc_json_bool_new(sc_allocator_t *alloc, bool val);
sc_json_value_t *sc_json_number_new(sc_allocator_t *alloc, double val);
sc_json_value_t *sc_json_string_new(sc_allocator_t *alloc, const char *s, size_t len);
sc_json_value_t *sc_json_array_new(sc_allocator_t *alloc);
sc_json_value_t *sc_json_object_new(sc_allocator_t *alloc);

sc_error_t sc_json_array_push(sc_allocator_t *alloc, sc_json_value_t *arr, sc_json_value_t *val);
sc_error_t sc_json_object_set(sc_allocator_t *alloc, sc_json_value_t *obj,
                              const char *key, sc_json_value_t *val);
sc_json_value_t *sc_json_object_get(const sc_json_value_t *obj, const char *key);
bool sc_json_object_remove(sc_allocator_t *alloc, sc_json_value_t *obj, const char *key);

const char *sc_json_get_string(const sc_json_value_t *val, const char *key);
double sc_json_get_number(const sc_json_value_t *val, const char *key, double default_val);
bool sc_json_get_bool(const sc_json_value_t *val, const char *key, bool default_val);

sc_error_t sc_json_stringify(sc_allocator_t *alloc, const sc_json_value_t *val,
                             char **out, size_t *out_len);

typedef struct sc_json_buf {
    char *ptr;
    size_t len;
    size_t cap;
    sc_allocator_t *alloc;
} sc_json_buf_t;

sc_error_t sc_json_buf_init(sc_json_buf_t *buf, sc_allocator_t *alloc);
void sc_json_buf_free(sc_json_buf_t *buf);
sc_error_t sc_json_buf_append_raw(sc_json_buf_t *buf, const char *str, size_t str_len);
sc_error_t sc_json_append_string(sc_json_buf_t *buf, const char *str, size_t str_len);
sc_error_t sc_json_append_key(sc_json_buf_t *buf, const char *key, size_t key_len);
sc_error_t sc_json_append_key_value(sc_json_buf_t *buf, const char *key, size_t key_len,
                                    const char *value, size_t value_len);
sc_error_t sc_json_append_key_int(sc_json_buf_t *buf, const char *key, size_t key_len, long long value);
sc_error_t sc_json_append_key_bool(sc_json_buf_t *buf, const char *key, size_t key_len, bool value);

#endif
