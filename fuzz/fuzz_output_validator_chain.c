/* libFuzzer harness for hu_output_validator_chain_execute.
 *
 * Builds the default outbound validator chain (all built-in validators) and
 * runs arbitrary input through it.  Must not crash or trigger ASan/UBSan
 * errors on any input. */
#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_output_validator_chain_t *chain = NULL;
    if (hu_validators_build_default_outbound_chain(&alloc, "Seth", 4, &chain) != HU_OK)
        return 0;
    /* First half — exercises ownership transitions from a partial input. */
    size_t half = size / 2;
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    hu_output_validator_chain_execute(chain, &alloc, NULL, (const char *)data, half, &cr);
    hu_chain_result_free(&alloc, &cr);

    /* Second half — reuses the same chain instance to cover REJECT/REWRITE across calls. */
    memset(&cr, 0, sizeof(cr));
    hu_output_validator_chain_execute(chain, &alloc, NULL, (const char *)data + half, size - half,
                                      &cr);
    hu_chain_result_free(&alloc, &cr);

    hu_output_validator_chain_destroy(chain);
    return 0;
}
