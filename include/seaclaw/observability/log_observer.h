#ifndef SC_LOG_OBSERVER_H
#define SC_LOG_OBSERVER_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/observer.h"
#include <stdio.h>

/**
 * Create a structured log observer that writes JSON lines to a FILE.
 * Output defaults to stderr if output is NULL.
 * Caller must call sc_observer's deinit when done; ctx is allocated via alloc.
 */
sc_observer_t sc_log_observer_create(sc_allocator_t *alloc, FILE *output);

#endif /* SC_LOG_OBSERVER_H */
