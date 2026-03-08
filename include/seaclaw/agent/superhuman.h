#ifndef SC_SUPERHUMAN_H
#define SC_SUPERHUMAN_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

#define SC_SUPERHUMAN_MAX_SERVICES 16

typedef struct sc_superhuman_service {
    const char *name;
    sc_error_t (*build_context)(void *ctx, sc_allocator_t *alloc, char **out, size_t *out_len);
    sc_error_t (*observe)(void *ctx, sc_allocator_t *alloc, const char *text, size_t text_len,
                          const char *role, size_t role_len);
    void *ctx;
} sc_superhuman_service_t;

typedef struct sc_superhuman_registry {
    sc_superhuman_service_t services[SC_SUPERHUMAN_MAX_SERVICES];
    size_t count;
} sc_superhuman_registry_t;

sc_error_t sc_superhuman_registry_init(sc_superhuman_registry_t *reg);
sc_error_t sc_superhuman_register(sc_superhuman_registry_t *reg, sc_superhuman_service_t service);
sc_error_t sc_superhuman_build_context(sc_superhuman_registry_t *reg, sc_allocator_t *alloc,
                                        char **out, size_t *out_len);
sc_error_t sc_superhuman_observe_all(sc_superhuman_registry_t *reg, sc_allocator_t *alloc,
                                      const char *text, size_t text_len, const char *role,
                                      size_t role_len);

#endif /* SC_SUPERHUMAN_H */
