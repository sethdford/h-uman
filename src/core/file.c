#include "human/core/file.h"

#include <errno.h>
#include <stdio.h>

hu_error_t hu_file_slurp(hu_allocator_t *alloc, const char *path, size_t max_bytes, char **out,
                         size_t *out_len) {
    if (!alloc || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (out_len)
        *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (!f)
        return (errno == ENOENT) ? HU_ERR_NOT_FOUND : HU_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    size_t len = (size_t)sz;
    if (max_bytes > 0 && len > max_bytes) {
        fclose(f);
        return HU_ERR_LIMIT_REACHED;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1u);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (len > 0 && fread(buf, 1u, len, f) != len) {
        fclose(f);
        alloc->free(alloc->ctx, buf, len + 1u);
        return HU_ERR_IO;
    }
    fclose(f);
    buf[len] = '\0';
    *out = buf;
    if (out_len)
        *out_len = len;
    return HU_OK;
}
