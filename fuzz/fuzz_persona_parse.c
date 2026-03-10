/* libFuzzer harness for hu_persona_load_json.
 * Fuzzes persona JSON parsing with random input.
 * Goal: find parsing crashes, leaks, or OOB reads.
 * Requires HU_HAS_PERSONA (HU_ENABLE_PERSONA). */
#include "human/core/allocator.h"
#include "human/persona.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
#if defined(HU_HAS_PERSONA) && HU_HAS_PERSONA
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, (const char *)data, size, &p);
    if (err == HU_OK)
        hu_persona_deinit(&alloc, &p);
#endif
    return 0;
}
