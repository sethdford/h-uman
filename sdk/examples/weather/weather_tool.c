/*
 * Example: Weather tool
 *
 * Full working tool with JSON params. Shows:
 * - parameters_json schema
 * - hu_json_get_string / hu_json_get_number for args
 * - hu_tool_result_ok_owned for allocated output
 * - HU_IS_TEST guard for deterministic tests
 */
#include "weather_tool.h"
#include "human/tool.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define HU_WEATHER_NAME "weather"
#define HU_WEATHER_DESC "Get current weather for a city. Returns temperature, conditions, and humidity."
#define HU_WEATHER_PARAMS "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\",\"description\":\"City name\"},\"units\":{\"type\":\"string\",\"enum\":[\"celsius\",\"fahrenheit\"],\"default\":\"celsius\"}},\"required\":[\"city\"]}"

typedef struct hu_weather_ctx {
    int _reserved; /* placeholder; could hold API key, cache, etc. */
} hu_weather_ctx_t;

static hu_error_t weather_execute(void *ctx, hu_allocator_t *alloc,
    const hu_json_value_t *args,
    hu_tool_result_t *out)
{
    (void)ctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *city = hu_json_get_string(args, "city");
    if (!city || strlen(city) == 0) {
        *out = hu_tool_result_fail("Missing required 'city' parameter", 31);
        return HU_OK;
    }

    const char *units = hu_json_get_string(args, "units");
    if (!units) units = "celsius";
    int use_fahrenheit = (strcmp(units, "fahrenheit") == 0);

#if HU_IS_TEST
    char *msg = hu_sprintf(alloc, "Weather in %s: 20°C, partly cloudy (stub)", city);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, strlen(msg));
    return HU_OK;
#else
    /* Production: call OpenWeatherMap or similar. For demo, return mock. */
    char buf[256];
    int n;
    if (use_fahrenheit) {
        n = snprintf(buf, sizeof(buf),
            "Weather in %s: 68°F, partly cloudy. Humidity: 55%%. (mock - add API)", city);
    } else {
        n = snprintf(buf, sizeof(buf),
            "Weather in %s: 20°C, partly cloudy. Humidity: 55%%. (mock - add API)", city);
    }
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        *out = hu_tool_result_fail("format error", 12);
        return HU_OK;
    }
    char *msg = hu_strndup(alloc, buf, (size_t)n);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, (size_t)n);
    return HU_OK;
#endif
}

static const char *weather_name(void *ctx) {
    (void)ctx;
    return HU_WEATHER_NAME;
}

static const char *weather_description(void *ctx) {
    (void)ctx;
    return HU_WEATHER_DESC;
}

static const char *weather_parameters_json(void *ctx) {
    (void)ctx;
    return HU_WEATHER_PARAMS;
}

static void weather_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)alloc;
    if (ctx) free(ctx);
}

static const hu_tool_vtable_t weather_vtable = {
    .execute = weather_execute,
    .name = weather_name,
    .description = weather_description,
    .parameters_json = weather_parameters_json,
    .deinit = weather_deinit,
};

hu_error_t hu_weather_create(hu_allocator_t *alloc, hu_tool_t *out) {
    (void)alloc;
    if (!out) return HU_ERR_INVALID_ARGUMENT;

    hu_weather_ctx_t *ctx = (hu_weather_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return HU_ERR_OUT_OF_MEMORY;

    out->ctx = ctx;
    out->vtable = &weather_vtable;
    return HU_OK;
}
