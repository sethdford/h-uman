#include "human/agent/scheduler.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* W14 system-state probes.
 *
 * All probes are HU_IS_TEST aware: when the env vars HU_TEST_LOAD_PCT,
 * HU_TEST_BATTERY_PCT, HU_TEST_ON_AC, HU_TEST_QUIET_HOURS are set, the
 * probes return deterministic values regardless of host state.  This is
 * what makes the W14 test suite portable — the test runner never
 * touches /proc, IOKit, or GetSystemPowerStatus.
 *
 * Production wiring for the OS-level paths is intentionally minimal in
 * this commit:
 *   - macOS: would use host_statistics() for load and
 *     IOPSGetProvidingPowerSourceType() for battery; both pull in
 *     IOKit.framework which we deliberately don't link in core today.
 *   - Linux: would read /proc/loadavg + /sys/class/power_supply.
 *   - Windows: would call GetSystemPowerStatus().
 *
 * Without the env-var override the probes return -1/false today.  The
 * scheduler treats unknown-load as eligible (favors running jobs) so
 * the production paths can be added in a follow-up without changing
 * scheduler behavior.
 *
 * Quiet-hours: persona currently exposes time-of-day overlay strings
 * (`time_overlay_late_night` etc) but not a structured quiet-hours
 * window.  The probe accepts a persona pointer for future use; today
 * it only honors the env var. */

static int read_env_int(const char *name, int fallback) {
#ifdef HU_IS_TEST
    const char *v = getenv(name);
    if (v && *v) {
        char *end = NULL;
        long n = strtol(v, &end, 10);
        if (end != v)
            return (int)n;
    }
#else
    (void)name;
#endif
    return fallback;
}

static bool read_env_bool(const char *name, bool fallback) {
#ifdef HU_IS_TEST
    const char *v = getenv(name);
    if (v && *v) {
        if (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y')
            return true;
        if (v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N')
            return false;
    }
#else
    (void)name;
#endif
    return fallback;
}

int hu_scheduler_probe_load_pct(void) {
    return read_env_int("HU_TEST_LOAD_PCT", -1);
}

int hu_scheduler_probe_battery_pct(void) {
    return read_env_int("HU_TEST_BATTERY_PCT", -1);
}

bool hu_scheduler_probe_on_ac_power(void) {
    /* Default true: when the host can't tell, we'd rather run jobs
     * than silently never schedule.  Tests opt out via HU_TEST_ON_AC=0. */
    return read_env_bool("HU_TEST_ON_AC", true);
}

bool hu_scheduler_probe_quiet_hours(int64_t now_ms, const hu_persona_t *persona) {
    (void)now_ms;
    (void)persona;
    return read_env_bool("HU_TEST_QUIET_HOURS", false);
}
