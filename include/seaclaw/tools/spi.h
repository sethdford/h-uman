#ifndef SC_TOOLS_SPI_H
#define SC_TOOLS_SPI_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_spi_create(sc_allocator_t *alloc,
    const char *device, size_t device_len,
    sc_tool_t *out);

#endif
