/* src/core/time.c
 *
 * Time abstraction for test determinism.
 * Allows tests to override system time via hu_time_set_test_override_ms().
 */

#include "human/core/time.h"

#include <stdint.h>
#include <time.h>

#ifdef HU_IS_TEST
static int64_t test_override_ms = 0;
static int override_active = 0;
#endif

int64_t hu_time_get_current_ms(void) {
#ifdef HU_IS_TEST
    if (override_active && test_override_ms > 0)
        return test_override_ms;
#endif

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

#ifdef HU_IS_TEST
void hu_time_set_test_override_ms(int64_t ms) {
    test_override_ms = ms;
    override_active = (ms > 0) ? 1 : 0;
}
#endif
