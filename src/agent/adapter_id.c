#include "human/agent/adapter_id.h"
#include "human/core/error.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

hu_error_t hu_format_adapter_id(const char *method_name, size_t step_index, time_t when,
                                char *buf, size_t cap) {
    if (!method_name || !buf || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    struct tm tm;
    gmtime_r(&when, &tm);
    char datebuf[16];
    if (strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tm) == 0)
        return HU_ERR_INVALID_ARGUMENT;
    int written = snprintf(buf, cap, "%s-%s-step-%zu", datebuf, method_name, step_index);
    if (written <= 0 || (size_t)written >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}
