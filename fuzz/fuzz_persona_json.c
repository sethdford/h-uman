#include "human/core/allocator.h"
#include "human/persona.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, (const char *)data, size, &p);
    if (err == HU_OK)
        hu_persona_deinit(&alloc, &p);
    return 0;
}
