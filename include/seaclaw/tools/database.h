#ifndef SC_TOOLS_DATABASE_H
#define SC_TOOLS_DATABASE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"

sc_error_t sc_database_tool_create(sc_allocator_t *alloc,
    const char *default_db_path, sc_tool_t *out);

#endif
