#ifndef HU_WEATHER_FETCH_H
#define HU_WEATHER_FETCH_H

#include "human/agent/weather_awareness.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define HU_WEATHER_CACHE_TTL_SEC 1800 /* 30 minutes */

typedef struct hu_weather_cache {
    hu_weather_context_t data;
    char location[128];
    uint64_t fetched_at; /* epoch seconds */
} hu_weather_cache_t;

/* Fetch weather for a location. Uses cache if fresh (<30 min).
 * api_key can be NULL (reads HU_WEATHER_API_KEY env).
 * HU_IS_TEST: returns mock data without network. */
hu_error_t hu_weather_fetch(hu_allocator_t *alloc, const char *location, size_t location_len,
                            const char *api_key, hu_weather_context_t *out);

#endif
