#ifndef HU_SUPERHUMAN_H
#define HU_SUPERHUMAN_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#define HU_SUPERHUMAN_MAX_SERVICES 16

typedef struct hu_superhuman_service {
    const char *name;
    hu_error_t (*build_context)(void *ctx, hu_allocator_t *alloc, char **out, size_t *out_len);
    hu_error_t (*observe)(void *ctx, hu_allocator_t *alloc, const char *text, size_t text_len,
                          const char *role, size_t role_len);
    void *ctx;
} hu_superhuman_service_t;

typedef struct hu_superhuman_registry {
    hu_superhuman_service_t services[HU_SUPERHUMAN_MAX_SERVICES];
    size_t count;
} hu_superhuman_registry_t;

hu_error_t hu_superhuman_registry_init(hu_superhuman_registry_t *reg);
hu_error_t hu_superhuman_register(hu_superhuman_registry_t *reg, hu_superhuman_service_t service);
hu_error_t hu_superhuman_build_context(hu_superhuman_registry_t *reg, hu_allocator_t *alloc,
                                        char **out, size_t *out_len);
hu_error_t hu_superhuman_observe_all(hu_superhuman_registry_t *reg, hu_allocator_t *alloc,
                                      const char *text, size_t text_len, const char *role,
                                      size_t role_len);

#endif /* HU_SUPERHUMAN_H */
