#include "human/ml/ml_scripts_dir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile-time macro from CMakeLists.txt. If unset, leave undefined so the
 * env-var path is the only source — useful for installed binaries that ship
 * scripts under share/human/scripts and expect HU_ML_SCRIPTS_DIR to be set
 * at install time. */
#ifndef HU_ML_SCRIPTS_DIR
#define HU_ML_SCRIPTS_DIR ""
#endif

static int has_quote_or_meta(const char *s) {
    if (!s) return 1;
    for (const char *p = s; *p; p++) {
        if (*p == '\'' || *p == '"' || *p == '`' || *p == '$' || *p == '\\' || *p == '\n')
            return 1;
    }
    return 0;
}

hu_error_t hu_ml_resolve_script_path(const char *script_name, char *out, size_t cap) {
    if (!script_name || !*script_name || !out || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (has_quote_or_meta(script_name))
        return HU_ERR_INVALID_ARGUMENT;

    const char *dir = NULL;

    /* Source 1: env override. Highest priority — dev / test injection. */
    const char *env_dir = getenv("HU_ML_SCRIPTS_DIR");
    if (env_dir && env_dir[0])
        dir = env_dir;

    /* Source 2: compile-time macro. */
    if (!dir) {
        static const char compile_dir[] = HU_ML_SCRIPTS_DIR;
        if (compile_dir[0])
            dir = compile_dir;
    }

    /* Source 3: HU_PROJECT_ROOT/scripts — compat with src/feeds/apple.c. */
    char composed[1024];
    if (!dir) {
        const char *proj_root = getenv("HU_PROJECT_ROOT");
        if (proj_root && proj_root[0]) {
            int n = snprintf(composed, sizeof(composed), "%s/scripts", proj_root);
            if (n > 0 && (size_t)n < sizeof(composed))
                dir = composed;
        }
    }

    if (!dir)
        return HU_ERR_NOT_SUPPORTED;

    if (has_quote_or_meta(dir))
        return HU_ERR_INVALID_ARGUMENT;

    int written = snprintf(out, cap, "%s/%s", dir, script_name);
    if (written < 0)
        return HU_ERR_IO;
    if ((size_t)written >= cap)
        return HU_ERR_INVALID_ARGUMENT; /* buffer too small — matches existing rm_mlx_score:77 pattern */

    return HU_OK;
}
