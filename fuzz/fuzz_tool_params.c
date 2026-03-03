/* Fuzz harness for tool param parsing and execute.
 * Parses input as JSON, passes to report tool execute. Report is safe (no I/O).
 * Must not crash on any input. */
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/report.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536)
        return 0;

    sc_allocator_t alloc = sc_system_allocator();
    char *buf = (char *)alloc.alloc(alloc.ctx, size + 1);
    if (!buf)
        return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    sc_json_value_t *args = NULL;
    sc_error_t err = sc_json_parse(&alloc, buf, size, &args);
    alloc.free(alloc.ctx, buf, size + 1);

    if (err != SC_OK || !args) {
        return 0;
    }
    if (args->type != SC_JSON_OBJECT) {
        sc_json_free(&alloc, args);
        return 0;
    }

    sc_tool_t tool;
    err = sc_report_create(&alloc, &tool);
    if (err != SC_OK) {
        sc_json_free(&alloc, args);
        return 0;
    }

    sc_tool_result_t result;
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    (void)err;

    sc_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
    sc_json_free(&alloc, args);

    return 0;
}
