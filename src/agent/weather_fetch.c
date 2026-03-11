#include "human/agent/weather_fetch.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_IS_TEST

hu_error_t hu_weather_fetch(hu_allocator_t *alloc, const char *location, size_t location_len,
                            const char *api_key, hu_weather_context_t *out) {
    (void)alloc;
    (void)api_key;
    if (!location || location_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    snprintf(out->condition, sizeof(out->condition), "Sunny");
    out->temp_celsius = 22;
    out->feels_like_celsius = 21;
    out->humidity_pct = 45;
    out->has_alert = false;
    return HU_OK;
}

#elif defined(HU_ENABLE_CURL)

#include "human/core/http.h"

static hu_weather_cache_t g_cache = {0};

hu_error_t hu_weather_fetch(hu_allocator_t *alloc, const char *location, size_t location_len,
                            const char *api_key, hu_weather_context_t *out) {
    if (!alloc || !location || location_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    uint64_t now = (uint64_t)time(NULL);

    if (g_cache.fetched_at > 0 && (now - g_cache.fetched_at) < HU_WEATHER_CACHE_TTL_SEC) {
        size_t cmp_len = location_len < sizeof(g_cache.location) - 1
                             ? location_len
                             : sizeof(g_cache.location) - 1;
        if (strncmp(g_cache.location, location, cmp_len) == 0 &&
            g_cache.location[location_len < sizeof(g_cache.location) ? location_len : sizeof(g_cache.location) - 1] == '\0') {
            *out = g_cache.data;
            return HU_OK;
        }
    }

    const char *key = api_key;
    if (!key || key[0] == '\0')
        key = getenv("HU_WEATHER_API_KEY");
    if (!key || key[0] == '\0')
        return HU_ERR_NOT_SUPPORTED;

    char url[512];
    int ulen = snprintf(url, sizeof(url),
                        "https://api.openweathermap.org/data/2.5/weather?q=%.*s&appid=%s&units=metric",
                        (int)location_len, location, key);
    if (ulen <= 0 || (size_t)ulen >= sizeof(url))
        return HU_ERR_INVALID_ARGUMENT;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, NULL, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err;
    }
    if (resp.status_code != 200 || !resp.body) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    err = hu_weather_awareness_parse_json(resp.body, resp.body_len, out);
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    if (err != HU_OK)
        return err;

    g_cache.data = *out;
    size_t copy_len = location_len < sizeof(g_cache.location) - 1
                          ? location_len
                          : sizeof(g_cache.location) - 1;
    memcpy(g_cache.location, location, copy_len);
    g_cache.location[copy_len] = '\0';
    g_cache.fetched_at = now;

    return HU_OK;
}

#else

hu_error_t hu_weather_fetch(hu_allocator_t *alloc, const char *location, size_t location_len,
                            const char *api_key, hu_weather_context_t *out) {
    (void)alloc;
    (void)location;
    (void)location_len;
    (void)api_key;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

#endif
