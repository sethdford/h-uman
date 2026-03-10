#ifndef HU_MEMORY_FACTORY_H
#define HU_MEMORY_FACTORY_H

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/memory.h"

hu_memory_t hu_memory_create_from_config(hu_allocator_t *alloc, const hu_config_t *cfg,
                                         const char *workspace_dir);

#endif /* HU_MEMORY_FACTORY_H */
