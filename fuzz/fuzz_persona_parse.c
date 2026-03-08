/* libFuzzer harness for sc_persona_load_json.
 * Fuzzes persona JSON parsing with random input.
 * Goal: find parsing crashes, leaks, or OOB reads.
 * Requires SC_HAS_PERSONA (SC_ENABLE_PERSONA). */
#include "seaclaw/core/allocator.h"
#include "seaclaw/persona.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
#if defined(SC_HAS_PERSONA) && SC_HAS_PERSONA
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, (const char *)data, size, &p);
    if (err == SC_OK)
        sc_persona_deinit(&alloc, &p);
#endif
    return 0;
}
