/* libFuzzer harness for sc_web_search_url_encode. Must not crash on any input. */
#include "seaclaw/core/allocator.h"
#include "seaclaw/tools/web_search_providers.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_web_search_url_encode(&alloc, (const char *)data, size, &out, &out_len);
    if (err == SC_OK && out)
        alloc.free(alloc.ctx, out, out_len + 1);
    return 0;
}
