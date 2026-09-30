#ifndef HU_CONFIG_MUTATOR_H
#define HU_CONFIG_MUTATOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

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

/* Check if path requires daemon restart. */
bool hu_config_mutator_path_requires_restart(const char *path);

/* Every write to config.json goes through this module. A mutation edits one
 * path in the parsed file and re-renders the whole document, so keys it does
 * not touch survive — unlike re-serializing an hu_config_t, which drops every
 * key the serializer does not model. Paths are allowlisted, and a SET is
 * refused (HU_ERR_INVALID_ARGUMENT, nothing written) unless the patch passes
 * hu_config_validate_document. Callers pass the path the config was loaded
 * from (hu_config_t.config_path); this module never resolves a location
 * itself, so a write can only land on the file that was read. */

/* Mutate the config file at cfg_path. Caller frees *out with
 * hu_config_mutator_free_result. */
hu_error_t hu_config_mutator_mutate_at(hu_allocator_t *alloc, const char *cfg_path,
                                       hu_mutation_action_t action, const char *path,
                                       const char *value_raw, hu_mutation_options_t options,
                                       hu_mutation_result_t *out);

/* Replace the whole file with `raw`, byte for byte, after strict validation.
 * The previous contents are kept at "<cfg_path>.bak". */
hu_error_t hu_config_mutator_replace_at(hu_allocator_t *alloc, const char *cfg_path,
                                        const char *raw, size_t raw_len);

/* Render {"a":{"b":<value_json>}} for path "a.b". Caller frees *out (len + 1). */
hu_error_t hu_config_mutator_build_patch(hu_allocator_t *alloc, const char *path,
                                         const char *value_json, char **out, size_t *out_len);

#endif /* HU_CONFIG_MUTATOR_H */
