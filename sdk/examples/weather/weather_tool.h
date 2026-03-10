#ifndef HU_WEATHER_TOOL_H
#define HU_WEATHER_TOOL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

/**
 * Example: Weather tool — returns current conditions for a city.
 *
 * In HU_IS_TEST returns stub data. In production, could call
 * OpenWeatherMap or similar API (add API key handling).
 */
hu_error_t hu_weather_create(hu_allocator_t *alloc, hu_tool_t *out);

#endif /* HU_WEATHER_TOOL_H */
