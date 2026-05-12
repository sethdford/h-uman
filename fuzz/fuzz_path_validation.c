/* Fuzz harness for hu_tool_validate_path.
 * Exercises traversal detection, URL-encoded patterns, and workspace scoping.
 * Must not crash on any input. */
#include "human/tools/validation.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 8192)
        return 0;

    char path[8193];
    memcpy(path, data, size);
    path[size] = '\0';

    (void)hu_tool_validate_path(path, NULL, 0);

    static const char workspace[] = "/home/user/project";
    (void)hu_tool_validate_path(path, workspace, sizeof(workspace) - 1);

    return 0;
}
