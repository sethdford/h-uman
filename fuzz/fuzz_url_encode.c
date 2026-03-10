/* libFuzzer harness for hu_web_search_url_encode. Must not crash on any input. */
#include "human/core/allocator.h"
#include "human/tools/web_search_providers.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_web_search_url_encode(&alloc, (const char *)data, size, &out, &out_len);
    if (err == HU_OK && out)
        alloc.free(alloc.ctx, out, out_len + 1);
    return 0;
}
