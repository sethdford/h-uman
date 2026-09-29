/* calibration_chatdb.h — the one chat.db path resolver shared by the three
 * calibration analyzers (clone, style, timing). An explicit db_path wins;
 * otherwise hu_paths_chatdb() resolves $HU_CHATDB or the macOS default. */
#ifndef HU_CALIBRATION_CHATDB_H
#define HU_CALIBRATION_CHATDB_H

#include "human/core/error.h"
#include "human/core/paths.h"
#include <stddef.h>
#include <string.h>

static inline hu_error_t hu_calibration_resolve_chatdb(const char *db_path, char *out, size_t cap) {
    if (db_path && db_path[0]) {
        size_t len = strlen(db_path);
        if (len + 1 > cap)
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(out, db_path, len + 1);
        return HU_OK;
    }
#if defined(__APPLE__) && defined(__MACH__)
    int n = hu_paths_chatdb(out, cap);
    if (n < 0)
        return HU_ERR_NOT_FOUND;
    if ((size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
#else
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

#endif /* HU_CALIBRATION_CHATDB_H */
