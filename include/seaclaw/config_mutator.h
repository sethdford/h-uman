#ifndef SC_CONFIG_MUTATOR_H
#define SC_CONFIG_MUTATOR_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>

typedef enum sc_mutation_action {
    SC_MUTATION_SET,
    SC_MUTATION_UNSET,
} sc_mutation_action_t;

typedef struct sc_mutation_options {
    bool apply;
} sc_mutation_options_t;

typedef struct sc_mutation_result {
    char *path;
    bool changed;
    bool applied;
    bool requires_restart;
    char *old_value_json;
    char *new_value_json;
    char *backup_path;  /* nullable */
} sc_mutation_result_t;

void sc_config_mutator_free_result(sc_allocator_t *alloc, sc_mutation_result_t *result);

/* Default config path (~/.seaclaw/config.json). Caller must free. */
sc_error_t sc_config_mutator_default_path(sc_allocator_t *alloc, char **out_path);

/* Check if path requires daemon restart. */
bool sc_config_mutator_path_requires_restart(const char *path);

/* Get value at path as JSON string. Caller must free. */
sc_error_t sc_config_mutator_get_path_value_json(sc_allocator_t *alloc, const char *path, char **out_json);

/* Mutate config. Caller must free result with sc_config_mutator_free_result. */
sc_error_t sc_config_mutator_mutate(sc_allocator_t *alloc,
    sc_mutation_action_t action,
    const char *path,
    const char *value_raw,  /* nullable for unset */
    sc_mutation_options_t options,
    sc_mutation_result_t *out);

#endif /* SC_CONFIG_MUTATOR_H */
