#ifndef HU_AGENT_PROFILE_H
#define HU_AGENT_PROFILE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_agent_profile_type {
    HU_PROFILE_CODING,
    HU_PROFILE_OPS,
    HU_PROFILE_MESSAGING,
    HU_PROFILE_MINIMAL,
    HU_PROFILE_CUSTOM,
} hu_agent_profile_type_t;

typedef struct hu_agent_profile {
    hu_agent_profile_type_t type;
    const char *name;
    const char *description;
    const char *system_prompt;
    const char *const *default_tools;
    size_t default_tools_count;
    const char *preferred_model;
    double temperature;
    uint32_t max_iterations;
    uint32_t max_history;
    uint8_t autonomy_level;
} hu_agent_profile_t;

const hu_agent_profile_t *hu_agent_profile_get(hu_agent_profile_type_t type);

const hu_agent_profile_t *hu_agent_profile_by_name(const char *name, size_t name_len);

hu_error_t hu_agent_profile_list(const hu_agent_profile_t **out, size_t *count);

#endif /* HU_AGENT_PROFILE_H */
