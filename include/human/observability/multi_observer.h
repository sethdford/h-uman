#ifndef HU_MULTI_OBSERVER_H
#define HU_MULTI_OBSERVER_H

#include "human/core/allocator.h"
#include "human/observer.h"
#include <stddef.h>

/**
 * Create a fan-out observer that forwards events/metrics to all given observers.
 * Observers array is copied; caller keeps ownership of the observer instances.
 * Caller must call hu_observer's deinit when done; ctx is allocated via alloc.
 */
hu_observer_t hu_multi_observer_create(hu_allocator_t *alloc, const hu_observer_t *observers,
                                       size_t count);

#endif /* HU_MULTI_OBSERVER_H */
