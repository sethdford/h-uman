#include "human/core/file_util.h"

#include <stdio.h>

hu_error_t hu_file_read_all(hu_allocator_t *alloc, const char *path, size_t max_bytes,
                            char **out_data, size_t *out_len) {
    if (!alloc || !path || !out_data || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    if (sz == 0 || (max_bytes > 0 && (size_t)sz > max_bytes)) {
        fclose(f);
        return HU_ERR_INVALID_FORMAT;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_IO;
    }
    buf[rd] = '\0';

    *out_data = buf;
    *out_len = rd;
    return HU_OK;
}
