#ifndef SC_CONFIG_PARSE_H
#define SC_CONFIG_PARSE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include <stddef.h>

/**
 * Parse JSON array of strings into allocated array.
 * Caller owns result and each string; must free array and each element.
 */
sc_error_t sc_config_parse_string_array(sc_allocator_t *alloc,
    const sc_json_value_t *arr,
    char ***out_strings, size_t *out_count);

#endif /* SC_CONFIG_PARSE_H */
