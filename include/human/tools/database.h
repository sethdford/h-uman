#ifndef HU_TOOLS_DATABASE_H
#define HU_TOOLS_DATABASE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"

hu_error_t hu_database_tool_create(hu_allocator_t *alloc, const char *default_db_path,
                                   hu_tool_t *out);

#endif
