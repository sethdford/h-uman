#include "human/core/state_file.h"
#include <stdlib.h>

const char *hu_state_file_default_path(const char *filename, char *buf, size_t cap) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)filename;
    (void)buf;
    (void)cap;
    return NULL; /* tests never touch the real home dir */
#else
    const char *home = getenv("HOME");
    if (!home || !home[0] || !filename || !filename[0] || !buf || cap == 0)
        return NULL;
    int n = snprintf(buf, cap, "%s/.human/%s", home, filename);
    if (n <= 0 || (size_t)n >= cap)
        return NULL;
    return buf;
#endif
}

FILE *hu_state_file_write_begin(const char *path, char *tmp, size_t tmp_cap) {
    if (!path || !path[0] || !tmp || tmp_cap == 0)
        return NULL;
    int n = snprintf(tmp, tmp_cap, "%s.tmp", path);
    if (n <= 0 || (size_t)n >= tmp_cap)
        return NULL;
    return fopen(tmp, "w");
}

hu_error_t hu_state_file_write_commit(FILE *f, bool write_ok, const char *tmp, const char *path) {
    if (!f || !tmp || !path)
        return HU_ERR_INVALID_ARGUMENT;
    if (fclose(f) != 0 || !write_ok) {
        remove(tmp);
        return HU_ERR_IO;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return HU_ERR_IO;
    }
    return HU_OK;
}
