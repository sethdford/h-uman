#include "human/core/allocator.h"
#include "human/core/json.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536)
        return 0;

    hu_allocator_t alloc = hu_system_allocator();
    char *buf = alloc.alloc(alloc.ctx, size + 1);
    if (!buf)
        return 0;
    __builtin_memcpy(buf, data, size);
    buf[size] = '\0';

    hu_json_value_t *val = NULL;
    hu_json_parse(&alloc, buf, size, &val);
    if (val) {
        hu_json_free(&alloc, val);
    }
    alloc.free(alloc.ctx, buf, size + 1);
    return 0;
}
