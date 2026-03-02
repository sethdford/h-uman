#ifndef SC_PROVIDERS_FACTORY_H
#define SC_PROVIDERS_FACTORY_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include <stddef.h>

sc_error_t sc_provider_create(sc_allocator_t *alloc,
    const char *name, size_t name_len,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out);

#endif /* SC_PROVIDERS_FACTORY_H */
