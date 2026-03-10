#ifndef HU_TOOLS_I2C_H
#define HU_TOOLS_I2C_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_i2c_create(hu_allocator_t *alloc, const char *serial_port, size_t serial_port_len,
                         hu_tool_t *out);

#endif
