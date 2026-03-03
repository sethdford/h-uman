#ifndef SC_MEMORY_FACTORY_H
#define SC_MEMORY_FACTORY_H

#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/memory.h"

sc_memory_t sc_memory_create_from_config(sc_allocator_t *alloc, const sc_config_t *cfg,
                                         const char *workspace_dir);

#endif /* SC_MEMORY_FACTORY_H */
