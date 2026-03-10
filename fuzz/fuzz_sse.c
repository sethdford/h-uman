/* libFuzzer harness for hu_sse_parser_feed. Must not crash on any input. */
#include "human/core/allocator.h"
#include "human/providers/sse.h"
#include <stddef.h>
#include <stdint.h>

#define HU_FUZZ_SSE_MAX_INPUT 65536

static void sse_count_cb(const char *event_type, size_t event_type_len, const char *data,
                         size_t data_len, void *userdata) {
    (void)event_type;
    (void)event_type_len;
    (void)data;
    (void)data_len;
    (void)userdata;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > HU_FUZZ_SSE_MAX_INPUT)
        return 0;

    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_error_t err = hu_sse_parser_init(&p, &alloc);
    if (err != HU_OK)
        return 0;

    err = hu_sse_parser_feed(&p, (const char *)data, size, sse_count_cb, NULL);
    (void)err;

    hu_sse_parser_deinit(&p);
    return 0;
}
