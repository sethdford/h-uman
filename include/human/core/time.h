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

/**
 * hu_time_get_current_ms() — Return current time in milliseconds since epoch.
 *
 * Production path: clock_gettime(CLOCK_MONOTONIC).
 * Test path: returns override value if hu_time_set_test_override_ms() was called.
 */
int64_t hu_time_get_current_ms(void);

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
