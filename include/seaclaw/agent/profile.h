#ifndef SC_AGENT_PROFILE_H
#define SC_AGENT_PROFILE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum sc_agent_profile_type {
    SC_PROFILE_CODING,
    SC_PROFILE_OPS,
    SC_PROFILE_MESSAGING,
    SC_PROFILE_MINIMAL,
    SC_PROFILE_CUSTOM,
} sc_agent_profile_type_t;

typedef struct sc_agent_profile {
    sc_agent_profile_type_t type;
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
} sc_agent_profile_t;

const sc_agent_profile_t *sc_agent_profile_get(sc_agent_profile_type_t type);

const sc_agent_profile_t *sc_agent_profile_by_name(const char *name, size_t name_len);

sc_error_t sc_agent_profile_list(const sc_agent_profile_t **out, size_t *count);

#endif /* SC_AGENT_PROFILE_H */
