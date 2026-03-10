#include "human/tools/validation.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 4096)
        return 0;

    char *buf = malloc(size + 1);
    if (!buf)
        return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    (void)hu_tool_validate_url(buf);

    free(buf);
    return 0;
}
