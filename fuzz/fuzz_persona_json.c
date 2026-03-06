#include "seaclaw/core/allocator.h"
#include "seaclaw/persona.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, (const char *)data, size, &p);
    if (err == SC_OK)
        sc_persona_deinit(&alloc, &p);
    return 0;
}
