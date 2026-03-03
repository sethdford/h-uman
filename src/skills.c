#include "seaclaw/skills.h"
#include "seaclaw/core/string.h"
#include <string.h>

sc_error_t sc_skills_list(sc_allocator_t *alloc, const char *workspace_dir, sc_skill_t **out_skills,
                          size_t *out_count) {
    if (!alloc || !out_skills || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_skills = NULL;
    *out_count = 0;

    sc_skillforge_t sf;
    sc_error_t err = sc_skillforge_create(alloc, &sf);
    if (err != SC_OK)
        return err;

    const char *dir = workspace_dir ? workspace_dir : ".";
    err = sc_skillforge_discover(&sf, dir);
    if (err != SC_OK) {
        sc_skillforge_destroy(&sf);
        return err;
    }

    sc_skill_t *from = NULL;
    size_t n = 0;
    err = sc_skillforge_list_skills(&sf, &from, &n);
    if (err != SC_OK || n == 0) {
        sc_skillforge_destroy(&sf);
        return err;
    }

    sc_skill_t *copy = (sc_skill_t *)alloc->alloc(alloc->ctx, n * sizeof(sc_skill_t));
    if (!copy) {
        sc_skillforge_destroy(&sf);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(copy, 0, n * sizeof(sc_skill_t));

    for (size_t i = 0; i < n; i++) {
        copy[i].name = sc_strdup(alloc, from[i].name);
        copy[i].description =
            from[i].description ? sc_strdup(alloc, from[i].description) : sc_strdup(alloc, "");
        copy[i].parameters = from[i].parameters ? sc_strdup(alloc, from[i].parameters) : NULL;
        copy[i].enabled = from[i].enabled;
        if (!copy[i].name || !copy[i].description) {
            sc_skills_free(alloc, copy, n);
            sc_skillforge_destroy(&sf);
            return SC_ERR_OUT_OF_MEMORY;
        }
    }

    sc_skillforge_destroy(&sf);
    *out_skills = copy;
    *out_count = n;
    return SC_OK;
}

void sc_skills_free(sc_allocator_t *alloc, sc_skill_t *skills, size_t count) {
    if (!alloc || !skills)
        return;
    for (size_t i = 0; i < count; i++) {
        if (skills[i].name)
            alloc->free(alloc->ctx, skills[i].name, strlen(skills[i].name) + 1);
        if (skills[i].description)
            alloc->free(alloc->ctx, skills[i].description, strlen(skills[i].description) + 1);
        if (skills[i].parameters)
            alloc->free(alloc->ctx, skills[i].parameters, strlen(skills[i].parameters) + 1);
    }
    alloc->free(alloc->ctx, skills, count * sizeof(sc_skill_t));
}
