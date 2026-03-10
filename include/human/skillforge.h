#ifndef HU_SKILLFORGE_H
#define HU_SKILLFORGE_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * Skill — a discovered skill with manifest metadata.
 */
typedef struct hu_skill {
    char *name;
    char *description;
    char *parameters; /* JSON string, may be NULL */
    bool enabled;
} hu_skill_t;

/**
 * SkillForge — skill discovery and integration registry.
 */
typedef struct hu_skillforge {
    hu_skill_t *skills;
    size_t skills_len;
    size_t skills_cap;
    hu_allocator_t *alloc;
} hu_skillforge_t;

hu_error_t hu_skillforge_create(hu_allocator_t *alloc, hu_skillforge_t *out);
void hu_skillforge_destroy(hu_skillforge_t *sf);

/**
 * Discover skills by scanning a directory for *.skill.json files.
 * In HU_IS_TEST mode, uses test data instead of scanning.
 */
hu_error_t hu_skillforge_discover(hu_skillforge_t *sf, const char *dir_path);

/**
 * Lookup a skill by name. Returns NULL if not found.
 */
hu_skill_t *hu_skillforge_get_skill(const hu_skillforge_t *sf, const char *name);

/**
 * List all registered skills. Output is owned by sf; valid until destroy.
 */
hu_error_t hu_skillforge_list_skills(const hu_skillforge_t *sf, hu_skill_t **out,
                                     size_t *out_count);

hu_error_t hu_skillforge_enable(hu_skillforge_t *sf, const char *name);
hu_error_t hu_skillforge_disable(hu_skillforge_t *sf, const char *name);

hu_error_t hu_skillforge_execute(hu_allocator_t *alloc, const hu_skillforge_t *sf, const char *name,
                                 char **out_instructions);

hu_error_t hu_skillforge_install(const char *name, const char *url);

hu_error_t hu_skillforge_uninstall(hu_skillforge_t *sf, const char *name);

#endif /* HU_SKILLFORGE_H */
