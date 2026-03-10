#ifndef HU_TOOLS_SPI_H
#define HU_TOOLS_SPI_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_spi_create(hu_allocator_t *alloc, const char *device, size_t device_len,
                         hu_tool_t *out);

#endif
