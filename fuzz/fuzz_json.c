/* libFuzzer harness for hu_json_parse. Must not crash on any input. */
#include "human/core/allocator.h"
#include "human/core/json.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(&alloc, (const char *)data, size, &root);
    if (err == HU_OK && root)
        hu_json_free(&alloc, root);
    return 0;
}
