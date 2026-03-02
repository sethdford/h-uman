#ifndef SC_TOOLS_HARDWARE_INFO_H
#define SC_TOOLS_HARDWARE_INFO_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stdbool.h>
#include <stddef.h>

sc_error_t sc_hardware_info_create(sc_allocator_t *alloc, bool enabled, sc_tool_t *out);

#endif
