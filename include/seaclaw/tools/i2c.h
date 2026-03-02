#ifndef SC_TOOLS_I2C_H
#define SC_TOOLS_I2C_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_i2c_create(sc_allocator_t *alloc,
    const char *serial_port, size_t serial_port_len,
    sc_tool_t *out);

#endif
