#ifndef SC_SKILLFORGE_H
#define SC_SKILLFORGE_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * Skill — a discovered skill with manifest metadata.
 */
typedef struct sc_skill {
    char *name;
    char *description;
    char *parameters;  /* JSON string, may be NULL */
    bool enabled;
} sc_skill_t;

/**
 * SkillForge — skill discovery and integration registry.
 */
typedef struct sc_skillforge {
    sc_skill_t *skills;
    size_t skills_len;
    size_t skills_cap;
    sc_allocator_t *alloc;
} sc_skillforge_t;

sc_error_t sc_skillforge_create(sc_allocator_t *alloc, sc_skillforge_t *out);
void sc_skillforge_destroy(sc_skillforge_t *sf);

/**
 * Discover skills by scanning a directory for *.skill.json files.
 * In SC_IS_TEST mode, uses test data instead of scanning.
 */
sc_error_t sc_skillforge_discover(sc_skillforge_t *sf, const char *dir_path);

/**
 * Lookup a skill by name. Returns NULL if not found.
 */
sc_skill_t *sc_skillforge_get_skill(const sc_skillforge_t *sf, const char *name);

/**
 * List all registered skills. Output is owned by sf; valid until destroy.
 */
sc_error_t sc_skillforge_list_skills(const sc_skillforge_t *sf,
    sc_skill_t **out, size_t *out_count);

sc_error_t sc_skillforge_enable(sc_skillforge_t *sf, const char *name);
sc_error_t sc_skillforge_disable(sc_skillforge_t *sf, const char *name);

sc_error_t sc_skillforge_execute(sc_allocator_t *alloc, const sc_skillforge_t *sf,
    const char *name, char **out_instructions);

sc_error_t sc_skillforge_install(const char *name, const char *url);

sc_error_t sc_skillforge_uninstall(sc_skillforge_t *sf, const char *name);

#endif /* SC_SKILLFORGE_H */
