#include "seaclaw/core/json.h"
#include "seaclaw/core/allocator.h"
#include <stdint.h>
#include <stddef.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536) return 0;

    sc_allocator_t alloc = sc_system_allocator();
    char *buf = alloc.alloc(alloc.ctx, size + 1);
    if (!buf) return 0;
    __builtin_memcpy(buf, data, size);
    buf[size] = '\0';

    sc_json_value_t *val = NULL;
    sc_json_parse(&alloc, buf, size, &val);
    if (val) {
        sc_json_free(&alloc, val);
    }
    alloc.free(alloc.ctx, buf, size + 1);
    return 0;
}
