/* include/human/core/time.h
 *
 * Time abstraction for test determinism.
 * Production calls clock_gettime(); tests can override via hu_time_set_test_override_ms().
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * hu_time_get_current_ms() — MONOTONIC milliseconds (CLOCK_MONOTONIC), for
 * intervals, deadlines and rate limits. NOT wall-clock: never store it as a
 * timestamp. (The old doc said "since epoch"; a 2026-09-12 dedupe that
 * believed it turned graph first_seen stamps into uptime values.)
 *
 * Test path: returns the override value if hu_time_set_test_override_ms() was called.
 */
int64_t hu_time_get_current_ms(void);

/*
 * hu_time_wall_ms() — WALL-CLOCK milliseconds since the Unix epoch
 * (CLOCK_REALTIME, falls back to time(NULL)). Use for anything persisted or
 * compared with data timestamps. Not affected by the test override.
 */
int64_t hu_time_wall_ms(void);

#ifdef HU_IS_TEST
/**
 * hu_time_set_test_override_ms() — Override time source for testing.
 *
 * When HU_IS_TEST is defined, hu_time_get_current_ms() returns this value
 * instead of querying system time. Allows deterministic tests without
 * time-dependent flakes.
 *
 * Pass 0 to disable override and return to system time.
 */
void hu_time_set_test_override_ms(int64_t ms);
#endif

#ifdef __cplusplus
}
#endif
