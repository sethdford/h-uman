#include "seaclaw/config_parse.h"
#include "seaclaw/core/string.h"
#include <string.h>

sc_error_t sc_config_parse_string_array(sc_allocator_t *alloc,
    const sc_json_value_t *arr,
    char ***out_strings, size_t *out_count) {
    if (!alloc || !out_strings || !out_count) return SC_ERR_INVALID_ARGUMENT;
    *out_strings = NULL;
    *out_count = 0;

    if (!arr || arr->type != SC_JSON_ARRAY) return SC_OK;

    size_t n = 0;
    sc_json_value_t **items = arr->data.array.items;
    if (!items) return SC_OK;
    for (size_t i = 0; i < arr->data.array.len; i++) {
        if (items[i] && items[i]->type == SC_JSON_STRING) n++;
    }
    if (n == 0) return SC_OK;

    char **strings = (char **)alloc->alloc(alloc->ctx, sizeof(char *) * n);
    if (!strings) return SC_ERR_OUT_OF_MEMORY;

    size_t j = 0;
    for (size_t i = 0; i < arr->data.array.len && j < n; i++) {
        if (!items[i] || items[i]->type != SC_JSON_STRING) continue;
        const char *s = items[i]->data.string.ptr;
        size_t len = items[i]->data.string.len;
        strings[j] = len > 0 ? sc_strndup(alloc, s, len) : sc_strdup(alloc, "");
        if (!strings[j]) {
            while (j > 0) {
                j--;
                alloc->free(alloc->ctx, strings[j], strlen(strings[j]) + 1);
            }
            alloc->free(alloc->ctx, strings, sizeof(char *) * n);
            return SC_ERR_OUT_OF_MEMORY;
        }
        j++;
    }

    *out_strings = strings;
    *out_count = j;
    return SC_OK;
}
