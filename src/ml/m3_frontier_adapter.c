/* M3 frontier adapter stub — see include/human/ml/m3_frontier_adapter.h */

#include "human/ml/m3_frontier_adapter.h"

#include <stdio.h>
#include <string.h>

struct hu_m3_frontier_adapter {
    hu_allocator_t *alloc;
    uint32_t schema_version;
};

hu_error_t hu_m3_frontier_adapter_try_open(hu_allocator_t *alloc, const char *path, size_t path_len,
                                          hu_m3_frontier_adapter_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (!path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return HU_ERR_IO;

    unsigned char hdr[16];
    size_t n = fread(hdr, 1, sizeof(hdr), fp);
    fclose(fp);
    if (n < 8)
        return HU_ERR_IO;
    if (memcmp(hdr, HU_M3_ADAPTER_MAGIC, 8) != 0)
        return HU_ERR_IO;

    uint32_t ver = 0;
    if (n >= 12) {
        ver = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) |
              ((uint32_t)hdr[11] << 24);
    }
    if (ver == 0)
        ver = 1;

    hu_m3_frontier_adapter_t *a =
        (hu_m3_frontier_adapter_t *)alloc->alloc(alloc->ctx, sizeof(*a));
    if (!a)
        return HU_ERR_OUT_OF_MEMORY;
    memset(a, 0, sizeof(*a));
    a->alloc = alloc;
    a->schema_version = ver;
    *out = a;
    return HU_OK;
}

hu_error_t hu_m3_frontier_adapter_noop_infer(hu_m3_frontier_adapter_t *adapter) {
    (void)adapter;
    return HU_OK;
}

void hu_m3_frontier_adapter_close(hu_allocator_t *alloc, hu_m3_frontier_adapter_t *adapter) {
    if (!alloc || !adapter)
        return;
    alloc->free(alloc->ctx, adapter, sizeof(*adapter));
}

uint32_t hu_m3_frontier_adapter_schema_version(const hu_m3_frontier_adapter_t *adapter) {
    return adapter ? adapter->schema_version : 0;
}
