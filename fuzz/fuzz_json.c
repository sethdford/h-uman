/* libFuzzer harness for sc_json_parse. Must not crash on any input. */
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(&alloc, (const char *)data, size, &root);
    if (err == SC_OK && root)
        sc_json_free(&alloc, root);
    return 0;
}
