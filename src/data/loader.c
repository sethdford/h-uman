#include "human/data/loader.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Embedded data registry lookup function (defined in embedded_registry.c) */
typedef struct {
    const char *path;
    const unsigned char *data;
    size_t len;
} hu_embedded_data_result_t;

extern const hu_embedded_data_result_t *hu_embedded_data_lookup(const char *path);

#define HU_DATA_MAX_FILE_SIZE (1024 * 1024)  /* 1MB limit */

static hu_error_t hu_data_expand_home(const char *path, char *buf, size_t buflen) {
    const char *home = getenv("HOME");
    if (home == NULL)
        return HU_ERR_NOT_FOUND;

    int written = snprintf(buf, buflen, "%s/.human/data/%s", home, path);
    if (written < 0 || (size_t)written >= buflen)
        return HU_ERR_IO;

    return HU_OK;
}

hu_error_t hu_data_load_embedded(hu_allocator_t *alloc, const char *relative_path,
                                 char **out, size_t *out_len) {
    if (alloc == NULL || relative_path == NULL || out == NULL || out_len == NULL)
        return HU_ERR_INVALID_ARGUMENT;

    const hu_embedded_data_result_t *entry = hu_embedded_data_lookup(relative_path);
    if (entry == NULL)
        return HU_ERR_NOT_FOUND;

    /* Allocate and copy the embedded data */
    char *copy = (char *)alloc->alloc(alloc->ctx, entry->len + 1);
    if (copy == NULL)
        return HU_ERR_OUT_OF_MEMORY;

    memcpy(copy, entry->data, entry->len);
    copy[entry->len] = '\0';  /* null-terminate */

    *out = copy;
    *out_len = entry->len;
    return HU_OK;
}

hu_error_t hu_data_load(hu_allocator_t *alloc, const char *relative_path,
                        char **out, size_t *out_len) {
    if (alloc == NULL || relative_path == NULL || out == NULL || out_len == NULL)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    /* In test mode, only use embedded data */
    return hu_data_load_embedded(alloc, relative_path, out, out_len);
#endif

    /* Try user override first */
    char user_path[512];
    hu_error_t err = hu_data_expand_home(relative_path, user_path, sizeof(user_path));
    if (err == HU_OK) {
        FILE *f = fopen(user_path, "rb");
        if (f != NULL) {
            /* Check file size */
            if (fseek(f, 0, SEEK_END) != 0) {
                fclose(f);
                return HU_ERR_IO;
            }

            long size = ftell(f);
            if (size < 0 || size > (long)HU_DATA_MAX_FILE_SIZE) {
                fclose(f);
                return HU_ERR_IO;
            }

            if (fseek(f, 0, SEEK_SET) != 0) {
                fclose(f);
                return HU_ERR_IO;
            }

            /* Allocate and read */
            char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)size + 1);
            if (buf == NULL) {
                fclose(f);
                return HU_ERR_OUT_OF_MEMORY;
            }

            size_t read_bytes = fread(buf, 1, (size_t)size, f);
            fclose(f);

            if (read_bytes != (size_t)size) {
                alloc->free(alloc->ctx, buf, (size_t)size + 1);
                return HU_ERR_IO;
            }

            buf[size] = '\0';
            *out = buf;
            *out_len = (size_t)size;
            return HU_OK;
        }
    }

    /* Fall back to embedded */
    return hu_data_load_embedded(alloc, relative_path, out, out_len);
}
