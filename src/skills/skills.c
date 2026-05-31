#include "human/skills.h"
#include "human/core/string.h"
#include <string.h>

hu_error_t hu_skills_list(hu_allocator_t *alloc, const char *workspace_dir, hu_skill_t **out_skills,
                          size_t *out_count) {
    if (!alloc || !out_skills || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_skills = NULL;
    *out_count = 0;

    hu_skillforge_t sf;
    hu_error_t err = hu_skillforge_create(alloc, &sf);
    if (err != HU_OK)
        return err;

    const char *dir = workspace_dir ? workspace_dir : ".";
    err = hu_skillforge_discover(&sf, dir);
    if (err != HU_OK) {
        hu_skillforge_destroy(&sf);
        return err;
    }

    hu_skill_t *from = NULL;
    size_t n = 0;
    err = hu_skillforge_list_skills(&sf, &from, &n);
    if (err != HU_OK || n == 0) {
        hu_skillforge_destroy(&sf);
        return err;
    }

    hu_skill_t *copy = (hu_skill_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_skill_t));
    if (!copy) {
        hu_skillforge_destroy(&sf);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(copy, 0, n * sizeof(hu_skill_t));

    for (size_t i = 0; i < n; i++) {
        copy[i].name = hu_strdup(alloc, from[i].name);
        copy[i].description =
            from[i].description ? hu_strdup(alloc, from[i].description) : hu_strdup(alloc, "");
        copy[i].command = from[i].command ? hu_strdup(alloc, from[i].command) : NULL;
        copy[i].parameters = from[i].parameters ? hu_strdup(alloc, from[i].parameters) : NULL;
        copy[i].skill_dir = from[i].skill_dir ? hu_strdup(alloc, from[i].skill_dir) : NULL;
        copy[i].instructions_path =
            from[i].instructions_path ? hu_strdup(alloc, from[i].instructions_path) : NULL;
        copy[i].enabled = from[i].enabled;
        if (!copy[i].name || !copy[i].description ||
            (from[i].command && !copy[i].command) ||
            (from[i].parameters && !copy[i].parameters) ||
            (from[i].skill_dir && !copy[i].skill_dir) ||
            (from[i].instructions_path && !copy[i].instructions_path)) {
            hu_skills_free(alloc, copy, n);
            hu_skillforge_destroy(&sf);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    hu_skillforge_destroy(&sf);
    *out_skills = copy;
    *out_count = n;
    return HU_OK;
}

void hu_skills_free(hu_allocator_t *alloc, hu_skill_t *skills, size_t count) {
    if (!alloc || !skills)
        return;
    for (size_t i = 0; i < count; i++) {
        if (skills[i].name)
            alloc->free(alloc->ctx, skills[i].name, strlen(skills[i].name) + 1);
        if (skills[i].description)
            alloc->free(alloc->ctx, skills[i].description, strlen(skills[i].description) + 1);
        if (skills[i].command)
            alloc->free(alloc->ctx, skills[i].command, strlen(skills[i].command) + 1);
        if (skills[i].parameters)
            alloc->free(alloc->ctx, skills[i].parameters, strlen(skills[i].parameters) + 1);
        if (skills[i].skill_dir)
            alloc->free(alloc->ctx, skills[i].skill_dir, strlen(skills[i].skill_dir) + 1);
        if (skills[i].instructions_path)
            alloc->free(alloc->ctx, skills[i].instructions_path,
                        strlen(skills[i].instructions_path) + 1);
    }
    alloc->free(alloc->ctx, skills, count * sizeof(hu_skill_t));
}
