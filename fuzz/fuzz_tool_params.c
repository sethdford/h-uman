/* Fuzz harness for tool param parsing and execute.
 * Parses input as JSON, passes to report tool execute. Report is safe (no I/O).
 * Must not crash on any input. */
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/tool.h"
#include "human/tools/report.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536)
        return 0;

    hu_allocator_t alloc = hu_system_allocator();
    char *buf = (char *)alloc.alloc(alloc.ctx, size + 1);
    if (!buf)
        return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    hu_json_value_t *args = NULL;
    hu_error_t err = hu_json_parse(&alloc, buf, size, &args);
    alloc.free(alloc.ctx, buf, size + 1);

    if (err != HU_OK || !args) {
        return 0;
    }
    if (args->type != HU_JSON_OBJECT) {
        hu_json_free(&alloc, args);
        return 0;
    }

    hu_tool_t tool;
    err = hu_report_create(&alloc, &tool);
    if (err != HU_OK) {
        hu_json_free(&alloc, args);
        return 0;
    }

    hu_tool_result_t result;
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    (void)err;

    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
    hu_json_free(&alloc, args);

    return 0;
}
