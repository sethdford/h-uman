#ifndef HU_GATEWAY_RATE_LIMIT_H
#define HU_GATEWAY_RATE_LIMIT_H

#include "human/core/allocator.h"
#include <stdbool.h>

typedef struct hu_rate_limiter hu_rate_limiter_t;

hu_rate_limiter_t *hu_rate_limiter_create(hu_allocator_t *alloc, int requests_per_window,
                                          int window_secs);
bool hu_rate_limiter_allow(hu_rate_limiter_t *lim, const char *ip);
void hu_rate_limiter_destroy(hu_rate_limiter_t *lim);

#endif /* HU_GATEWAY_RATE_LIMIT_H */
