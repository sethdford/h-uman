#ifndef HU_JSON_H
#define HU_JSON_H

#include "allocator.h"
#include "error.h"
#include "slice.h"
#include <stdbool.h>

typedef enum hu_json_type {
    HU_JSON_NULL,
    HU_JSON_BOOL,
    HU_JSON_NUMBER,
    HU_JSON_STRING,
    HU_JSON_ARRAY,
    HU_JSON_OBJECT
} hu_json_type_t;

typedef struct hu_json_value hu_json_value_t;
typedef struct hu_json_pair hu_json_pair_t;

struct hu_json_pair {
    char *key;
    size_t key_len;
    hu_json_value_t *value;
};

struct hu_json_value {
    hu_json_type_t type;
    union {
        bool boolean;
        double number;
        struct {
            char *ptr;
            size_t len;
        } string;
        struct {
            hu_json_value_t **items;
            size_t len;
            size_t cap;
        } array;
        struct {
            hu_json_pair_t *pairs;
            size_t len;
            size_t cap;
        } object;
    } data;
};

hu_error_t hu_json_parse(hu_allocator_t *alloc, const char *input, size_t input_len,
                         hu_json_value_t **out);
void hu_json_free(hu_allocator_t *alloc, hu_json_value_t *val);

hu_json_value_t *hu_json_null_new(hu_allocator_t *alloc);
hu_json_value_t *hu_json_bool_new(hu_allocator_t *alloc, bool val);
hu_json_value_t *hu_json_number_new(hu_allocator_t *alloc, double val);
hu_json_value_t *hu_json_string_new(hu_allocator_t *alloc, const char *s, size_t len);
hu_json_value_t *hu_json_array_new(hu_allocator_t *alloc);
hu_json_value_t *hu_json_object_new(hu_allocator_t *alloc);

hu_error_t hu_json_array_push(hu_allocator_t *alloc, hu_json_value_t *arr, hu_json_value_t *val);
hu_error_t hu_json_object_set(hu_allocator_t *alloc, hu_json_value_t *obj, const char *key,
                              hu_json_value_t *val);
hu_json_value_t *hu_json_object_get(const hu_json_value_t *obj, const char *key);
bool hu_json_object_remove(hu_allocator_t *alloc, hu_json_value_t *obj, const char *key);

const char *hu_json_get_string(const hu_json_value_t *val, const char *key);
double hu_json_get_number(const hu_json_value_t *val, const char *key, double default_val);
bool hu_json_get_bool(const hu_json_value_t *val, const char *key, bool default_val);

hu_error_t hu_json_stringify(hu_allocator_t *alloc, const hu_json_value_t *val, char **out,
                             size_t *out_len);

typedef struct hu_json_buf {
    char *ptr;
    size_t len;
    size_t cap;
    hu_allocator_t *alloc;
} hu_json_buf_t;

hu_error_t hu_json_buf_init(hu_json_buf_t *buf, hu_allocator_t *alloc);
void hu_json_buf_free(hu_json_buf_t *buf);
hu_error_t hu_json_buf_append_raw(hu_json_buf_t *buf, const char *str, size_t str_len);
hu_error_t hu_json_append_string(hu_json_buf_t *buf, const char *str, size_t str_len);
hu_error_t hu_json_append_key(hu_json_buf_t *buf, const char *key, size_t key_len);
hu_error_t hu_json_append_key_value(hu_json_buf_t *buf, const char *key, size_t key_len,
                                    const char *value, size_t value_len);
hu_error_t hu_json_append_key_int(hu_json_buf_t *buf, const char *key, size_t key_len,
                                  long long value);
hu_error_t hu_json_append_key_bool(hu_json_buf_t *buf, const char *key, size_t key_len, bool value);

#endif
