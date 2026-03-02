#ifndef SC_MULTI_OBSERVER_H
#define SC_MULTI_OBSERVER_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/observer.h"
#include <stddef.h>

/**
 * Create a fan-out observer that forwards events/metrics to all given observers.
 * Observers array is copied; caller keeps ownership of the observer instances.
 * Caller must call sc_observer's deinit when done; ctx is allocated via alloc.
 */
sc_observer_t sc_multi_observer_create(sc_allocator_t *alloc,
    const sc_observer_t *observers, size_t count);

#endif /* SC_MULTI_OBSERVER_H */
