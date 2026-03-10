/* libFuzzer harness for hu_fast_capture.
 * Fuzzes entity/emotion/topic extraction with random text.
 * Goal: find crashes in entity/emotion/topic extraction. */
#include "human/core/allocator.h"
#include "human/memory/fast_capture.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEXT_MAX 4096

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > TEXT_MAX)
        return 0;

    hu_allocator_t alloc = hu_system_allocator();
    hu_fc_result_t out;
    memset(&out, 0, sizeof(out));

    /* Ensure null-terminated; use alloc'd buffer for large inputs */
    char *text = NULL;
    if (size > 0) {
        text = (char *)alloc.alloc(alloc.ctx, size + 1);
        if (!text)
            return 0;
        memcpy(text, data, size);
        text[size] = '\0';
    }

    hu_error_t err = hu_fast_capture(&alloc, text ? text : "", size, &out);
    (void)err;

    hu_fc_result_deinit(&out, &alloc);
    if (text)
        alloc.free(alloc.ctx, text, size + 1);
    return 0;
}
