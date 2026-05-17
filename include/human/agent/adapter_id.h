#ifndef HU_AGENT_ADAPTER_ID_H
#define HU_AGENT_ADAPTER_ID_H

#include "human/core/error.h"
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_format_adapter_id(const char *method_name, size_t step_index, time_t when,
                                char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_ADAPTER_ID_H */
