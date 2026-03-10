#ifndef HU_CONFIG_PARSE_H
#define HU_CONFIG_PARSE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include <stddef.h>

/**
 * Parse JSON array of strings into allocated array.
 * Caller owns result and each string; must free array and each element.
 */
hu_error_t hu_config_parse_string_array(hu_allocator_t *alloc, const hu_json_value_t *arr,
                                        char ***out_strings, size_t *out_count);

#endif /* HU_CONFIG_PARSE_H */
