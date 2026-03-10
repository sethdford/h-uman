#ifndef HU_LOG_OBSERVER_H
#define HU_LOG_OBSERVER_H

#include "human/core/allocator.h"
#include "human/observer.h"
#include <stdio.h>

/**
 * Create a structured log observer that writes JSON lines to a FILE.
 * Output defaults to stderr if output is NULL.
 * Caller must call hu_observer's deinit when done; ctx is allocated via alloc.
 */
hu_observer_t hu_log_observer_create(hu_allocator_t *alloc, FILE *output);

#endif /* HU_LOG_OBSERVER_H */
