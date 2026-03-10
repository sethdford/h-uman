/* libFuzzer harness for the gateway control protocol JSON-RPC dispatch.
 * Feeds random bytes through hu_json_parse and exercises the RPC method
 * dispatch table used by hu_control_on_message.
 * Goal: find crashes, OOB reads, or leaks in RPC parsing. */
#include "human/core/allocator.h"
#include "human/core/json.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FUZZ_MAX_INPUT 8192

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > FUZZ_MAX_INPUT)
        return 0;

    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(&alloc, (const char *)data, size, &root);
    if (err != HU_OK || !root) {
        if (root)
            hu_json_free(&alloc, root);
        return 0;
    }

    /* Exercise the fields the control protocol reads */
    (void)hu_json_get_string(root, "type");
    (void)hu_json_get_string(root, "id");
    (void)hu_json_get_string(root, "method");

    const hu_json_value_t *params = hu_json_get(root, "params");
    if (params) {
        (void)hu_json_get_string(params, "session_key");
        (void)hu_json_get_string(params, "message");
        (void)hu_json_get_string(params, "key");
    }

    hu_json_free(&alloc, root);
    return 0;
}
