/*
 * Doctor auto-repair — creates missing directories, default configs, etc.
 */

#include "human/doctor_fix.h"
#include "human/core/string.h"
#include "human/skill_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HU_IS_TEST

hu_error_t hu_doctor_fix(hu_allocator_t *alloc, hu_config_t *cfg, hu_doctor_fix_result_t **results,
                         size_t *result_count) {
    (void)cfg;
    if (!alloc || !results || !result_count)
        return HU_ERR_INVALID_ARGUMENT;

    *result_count = 5;
    *results = (hu_doctor_fix_result_t *)alloc->alloc(alloc->ctx, sizeof(hu_doctor_fix_result_t) *
                                                                      (*result_count));
    if (!*results)
        return HU_ERR_OUT_OF_MEMORY;

    (*results)[0] = (hu_doctor_fix_result_t){"state directory", "exists (test mode)", true};
    (*results)[1] = (hu_doctor_fix_result_t){"skills directory", "exists (test mode)", true};
    (*results)[2] = (hu_doctor_fix_result_t){"plugins directory", "exists (test mode)", true};
    (*results)[3] = (hu_doctor_fix_result_t){"personas directory", "exists (test mode)", true};
    (*results)[4] = (hu_doctor_fix_result_t){"default config", "exists (test mode)", true};
    return HU_OK;
}

hu_error_t hu_doctor_fix_state_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = (hu_doctor_fix_result_t){"state directory", "ok (test)", true};
    return HU_OK;
}

hu_error_t hu_doctor_fix_skills_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = (hu_doctor_fix_result_t){"skills directory", "ok (test)", true};
    return HU_OK;
}

hu_error_t hu_doctor_fix_plugins_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = (hu_doctor_fix_result_t){"plugins directory", "ok (test)", true};
    return HU_OK;
}

hu_error_t hu_doctor_fix_personas_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = (hu_doctor_fix_result_t){"personas directory", "ok (test)", true};
    return HU_OK;
}

hu_error_t hu_doctor_fix_default_config(hu_allocator_t *alloc, hu_doctor_fix_result_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = (hu_doctor_fix_result_t){"default config", "ok (test)", true};
    return HU_OK;
}

#else /* !HU_IS_TEST */

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static bool ensure_dir(const char *path) {
#ifndef _WIN32
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return true;
    return mkdir(path, 0755) == 0;
#else
    (void)path;
    return false;
#endif
}

static bool get_human_dir(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;
    int n = snprintf(buf, cap, "%s/.human", home);
    return n > 0 && (size_t)n < cap;
}

hu_error_t hu_doctor_fix_state_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    char dir[512];
    if (!get_human_dir(dir, sizeof(dir))) {
        *out = (hu_doctor_fix_result_t){"state directory", "cannot resolve HOME", false};
        return HU_OK;
    }
    if (ensure_dir(dir))
        *out = (hu_doctor_fix_result_t){"state directory", "ok", true};
    else
        *out = (hu_doctor_fix_result_t){"state directory", "created", true};
    return HU_OK;
}

hu_error_t hu_doctor_fix_skills_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    char dir[512];
    if (!get_human_dir(dir, sizeof(dir))) {
        *out = (hu_doctor_fix_result_t){"skills directory", "cannot resolve HOME", false};
        return HU_OK;
    }
    char skills[560];
    snprintf(skills, sizeof(skills), "%s/skills", dir);
    ensure_dir(dir);
    if (ensure_dir(skills))
        *out = (hu_doctor_fix_result_t){"skills directory", "ok", true};
    else
        *out = (hu_doctor_fix_result_t){"skills directory", "failed to create", false};
    return HU_OK;
}

hu_error_t hu_doctor_fix_plugins_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    char dir[512];
    if (!get_human_dir(dir, sizeof(dir))) {
        *out = (hu_doctor_fix_result_t){"plugins directory", "cannot resolve HOME", false};
        return HU_OK;
    }
    char plugins[560];
    snprintf(plugins, sizeof(plugins), "%s/plugins", dir);
    ensure_dir(dir);
    if (ensure_dir(plugins))
        *out = (hu_doctor_fix_result_t){"plugins directory", "ok", true};
    else
        *out = (hu_doctor_fix_result_t){"plugins directory", "failed to create", false};
    return HU_OK;
}

hu_error_t hu_doctor_fix_personas_dir(hu_allocator_t *alloc, hu_doctor_fix_result_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    char dir[512];
    if (!get_human_dir(dir, sizeof(dir))) {
        *out = (hu_doctor_fix_result_t){"personas directory", "cannot resolve HOME", false};
        return HU_OK;
    }
    char personas[560];
    snprintf(personas, sizeof(personas), "%s/personas", dir);
    ensure_dir(dir);
    if (ensure_dir(personas))
        *out = (hu_doctor_fix_result_t){"personas directory", "ok", true};
    else
        *out = (hu_doctor_fix_result_t){"personas directory", "failed to create", false};
    return HU_OK;
}

hu_error_t hu_doctor_fix_default_config(hu_allocator_t *alloc, hu_doctor_fix_result_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    char dir[512];
    if (!get_human_dir(dir, sizeof(dir))) {
        *out = (hu_doctor_fix_result_t){"default config", "cannot resolve HOME", false};
        return HU_OK;
    }
    char cfg_path[560];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", dir);

#ifndef _WIN32
    struct stat st;
    if (stat(cfg_path, &st) == 0) {
        *out = (hu_doctor_fix_result_t){"default config", "exists", true};
        return HU_OK;
    }
#endif

    ensure_dir(dir);
    FILE *f = fopen(cfg_path, "w");
    if (!f) {
        *out = (hu_doctor_fix_result_t){"default config", "failed to create", false};
        return HU_OK;
    }
    fprintf(f, "{\n"
               "  \"default_provider\": \"ollama\",\n"
               "  \"default_model\": \"llama3\",\n"
               "  \"default_temperature\": 0.7,\n"
               "  \"memory_backend\": \"sqlite\",\n"
               "  \"gateway\": {\n"
               "    \"enabled\": false,\n"
               "    \"port\": 3000\n"
               "  }\n"
               "}\n");
    fclose(f);
    *out = (hu_doctor_fix_result_t){"default config", "created with defaults", true};
    return HU_OK;
}

hu_error_t hu_doctor_fix(hu_allocator_t *alloc, hu_config_t *cfg, hu_doctor_fix_result_t **results,
                         size_t *result_count) {
    (void)cfg;
    if (!alloc || !results || !result_count)
        return HU_ERR_INVALID_ARGUMENT;

    size_t cap = 5;
    *results =
        (hu_doctor_fix_result_t *)alloc->alloc(alloc->ctx, sizeof(hu_doctor_fix_result_t) * cap);
    if (!*results)
        return HU_ERR_OUT_OF_MEMORY;
    *result_count = 0;

    hu_doctor_fix_state_dir(alloc, &(*results)[(*result_count)++]);
    hu_doctor_fix_skills_dir(alloc, &(*results)[(*result_count)++]);
    hu_doctor_fix_plugins_dir(alloc, &(*results)[(*result_count)++]);
    hu_doctor_fix_personas_dir(alloc, &(*results)[(*result_count)++]);
    hu_doctor_fix_default_config(alloc, &(*results)[(*result_count)++]);

    return HU_OK;
}

#endif /* HU_IS_TEST */

void hu_doctor_fix_results_free(hu_allocator_t *alloc, hu_doctor_fix_result_t *results,
                                size_t count) {
    if (alloc && results)
        alloc->free(alloc->ctx, results, sizeof(hu_doctor_fix_result_t) * count);
}
