#ifndef HU_CONFIG_MUTATOR_H
#define HU_CONFIG_MUTATOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>

typedef enum hu_mutation_action {
    HU_MUTATION_SET,
    HU_MUTATION_UNSET,
} hu_mutation_action_t;

typedef struct hu_mutation_options {
    bool apply;
} hu_mutation_options_t;

typedef struct hu_mutation_result {
    char *path;
    bool changed;
    bool applied;
    bool requires_restart;
    char *old_value_json;
    char *new_value_json;
    char *backup_path; /* nullable */
} hu_mutation_result_t;

void hu_config_mutator_free_result(hu_allocator_t *alloc, hu_mutation_result_t *result);

/* Default config path (~/.human/config.json). Caller must free. */
hu_error_t hu_config_mutator_default_path(hu_allocator_t *alloc, char **out_path);

/* Check if path requires daemon restart. */
bool hu_config_mutator_path_requires_restart(const char *path);

/* Get value at path as JSON string. Caller must free. */
hu_error_t hu_config_mutator_get_path_value_json(hu_allocator_t *alloc, const char *path,
                                                 char **out_json);

/* Mutate config. Caller must free result with hu_config_mutator_free_result. */
hu_error_t hu_config_mutator_mutate(hu_allocator_t *alloc, hu_mutation_action_t action,
                                    const char *path,
                                    const char *value_raw, /* nullable for unset */
                                    hu_mutation_options_t options, hu_mutation_result_t *out);

#endif /* HU_CONFIG_MUTATOR_H */
