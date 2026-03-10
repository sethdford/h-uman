#ifndef HU_CONFIG_PARSE_INTERNAL_H
#define HU_CONFIG_PARSE_INTERNAL_H

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"

/* Shared helper: parse JSON array of strings into allocated array. */
hu_error_t parse_string_array(hu_allocator_t *a, char ***out, size_t *out_len,
                              const hu_json_value_t *arr);

/* Provider-related parsers */
hu_error_t parse_providers(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *arr);
hu_error_t parse_router(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj);

/* Channel-related parsers */
hu_error_t parse_channels(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj);

/* Agent-related parsers */
hu_error_t parse_agent(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj);

/* Behavior thresholds parser */
hu_error_t parse_behavior(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj);

#endif /* HU_CONFIG_PARSE_INTERNAL_H */
