#include "human/agent/scheduler.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(__APPLE__) && !defined(HU_IS_TEST)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <mach/host_info.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/processor_info.h>
#endif

#if defined(__linux__) && !defined(HU_IS_TEST)
#include <dirent.h>
#include <errno.h>
#include <sys/types.h>
#endif

/* W14 system-state probes.
 *
 * Two layers:
 *   1. Test override (HU_IS_TEST env vars: HU_TEST_LOAD_PCT,
 *      HU_TEST_BATTERY_PCT, HU_TEST_ON_AC, HU_TEST_QUIET_HOURS) so the
 *      W14 test suite never touches /proc, IOKit, or sysfs.
 *   2. Production OS reads:
 *        macOS — host_statistics() + IOPSCopyPowerSourcesInfo()
 *        Linux — /proc/loadavg + /sys/class/power_supply
 *        Other — returns -1/false; scheduler treats unknown-load as
 *                 eligible (it favors running jobs over silent skips).
 *
 * Quiet-hours: persona currently exposes time-of-day overlay strings
 * (`time_overlay_late_night` etc) but not a structured quiet-hours
 * window.  The probe accepts a persona pointer for future use; today
 * it falls back to the env override + a fixed 01:00–06:00 default
 * when persona is non-NULL (the sleep-compute sweet spot on most
 * personal devices). */

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

#if defined(__APPLE__) && !defined(HU_IS_TEST)

/* macOS: cumulative CPU ticks via host_statistics + a static prior so
 * the second call onward returns a *delta* load percentage. First call
 * after process start can't compute a delta — returns -1, which the
 * scheduler reads as "unknown, run jobs anyway." */
static int probe_load_macos(void) {
    static host_cpu_load_info_data_t prev = {0};
    static bool have_prev = false;
    host_cpu_load_info_data_t cur;
    mach_msg_type_number_t cnt = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&cur, &cnt) !=
        KERN_SUCCESS)
        return -1;
    if (!have_prev) {
        prev = cur;
        have_prev = true;
        return -1;
    }
    uint64_t user = cur.cpu_ticks[CPU_STATE_USER] - prev.cpu_ticks[CPU_STATE_USER];
    uint64_t sys  = cur.cpu_ticks[CPU_STATE_SYSTEM] - prev.cpu_ticks[CPU_STATE_SYSTEM];
    uint64_t nice = cur.cpu_ticks[CPU_STATE_NICE] - prev.cpu_ticks[CPU_STATE_NICE];
    uint64_t idle = cur.cpu_ticks[CPU_STATE_IDLE] - prev.cpu_ticks[CPU_STATE_IDLE];
    uint64_t total = user + sys + nice + idle;
    prev = cur;
    if (total == 0)
        return -1;
    int pct = (int)((100ULL * (user + sys + nice)) / total);
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    return pct;
}

/* macOS battery + AC via IOPSCopyPowerSourcesInfo. Desktops (no
 * battery source) report -1 / on_ac=true. */
static void probe_power_macos(int *out_battery_pct, bool *out_on_ac) {
    *out_battery_pct = -1;
    *out_on_ac = true;
    CFTypeRef blob = IOPSCopyPowerSourcesInfo();
    if (!blob)
        return;
    CFArrayRef list = IOPSCopyPowerSourcesList(blob);
    if (!list) {
        CFRelease(blob);
        return;
    }
    CFIndex n = CFArrayGetCount(list);
    for (CFIndex i = 0; i < n; i++) {
        CFTypeRef src = CFArrayGetValueAtIndex(list, i);
        CFDictionaryRef desc = IOPSGetPowerSourceDescription(blob, src);
        if (!desc)
            continue;
        CFNumberRef cur = (CFNumberRef)CFDictionaryGetValue(desc, CFSTR(kIOPSCurrentCapacityKey));
        CFNumberRef max = (CFNumberRef)CFDictionaryGetValue(desc, CFSTR(kIOPSMaxCapacityKey));
        if (cur && max) {
            int curv = 0, maxv = 0;
            CFNumberGetValue(cur, kCFNumberIntType, &curv);
            CFNumberGetValue(max, kCFNumberIntType, &maxv);
            if (maxv > 0)
                *out_battery_pct = (curv * 100) / maxv;
        }
        CFStringRef state =
            (CFStringRef)CFDictionaryGetValue(desc, CFSTR(kIOPSPowerSourceStateKey));
        if (state) {
            *out_on_ac = CFEqual(state, CFSTR(kIOPSACPowerValue));
        }
        break; /* first power source wins; laptops have one */
    }
    CFRelease(list);
    CFRelease(blob);
}

#endif /* __APPLE__ && !HU_IS_TEST */

#if defined(__linux__) && !defined(HU_IS_TEST)

/* Linux: /proc/loadavg gives the 1-minute load average; we map it to a
 * percentage relative to ncpu so 100% == "load == ncpu". Capped at 100
 * to keep the scheduler-eligibility math sane. */
static int probe_load_linux(void) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f)
        return -1;
    double avg1 = 0;
    int got = fscanf(f, "%lf", &avg1);
    fclose(f);
    if (got != 1)
        return -1;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu <= 0)
        ncpu = 1;
    double pct = (avg1 / (double)ncpu) * 100.0;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    return (int)pct;
}

/* Linux power-supply: scan /sys/class/power_supply for a Battery type
 * and any non-Battery type with online == 1. Returns -1 / true when the
 * host has no battery (most servers). */
static int read_int_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

static bool read_string_file_eq(const char *path, const char *expected) {
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        return false;
    buf[n] = '\0';
    /* trim trailing whitespace */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
        buf[n - 1] = '\0';
        n--;
    }
    return strcmp(buf, expected) == 0;
}

static void probe_power_linux(int *out_battery_pct, bool *out_on_ac) {
    *out_battery_pct = -1;
    *out_on_ac = true;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d)
        return;
    bool any_ac = false;
    bool any_ac_online = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char type_path[256];
        snprintf(type_path, sizeof(type_path), "/sys/class/power_supply/%s/type", e->d_name);
        FILE *f = fopen(type_path, "r");
        if (!f)
            continue;
        char type_buf[32];
        size_t n = fread(type_buf, 1, sizeof(type_buf) - 1, f);
        fclose(f);
        if (n == 0)
            continue;
        type_buf[n] = '\0';
        while (n > 0 && (type_buf[n - 1] == '\n' || type_buf[n - 1] == ' ')) {
            type_buf[n - 1] = '\0';
            n--;
        }
        if (strcmp(type_buf, "Battery") == 0) {
            char cap_path[256];
            snprintf(cap_path, sizeof(cap_path),
                     "/sys/class/power_supply/%s/capacity", e->d_name);
            int cap = read_int_file(cap_path);
            if (cap >= 0)
                *out_battery_pct = cap;
        } else if (strcmp(type_buf, "Mains") == 0 || strcmp(type_buf, "USB") == 0 ||
                   strcmp(type_buf, "ACA") == 0) {
            any_ac = true;
            char online_path[256];
            snprintf(online_path, sizeof(online_path),
                     "/sys/class/power_supply/%s/online", e->d_name);
            if (read_string_file_eq(online_path, "1"))
                any_ac_online = true;
        }
    }
    closedir(d);
    /* Only override the default true when we found at least one AC source
     * AND it reported offline; otherwise stay on default-true. */
    if (any_ac && !any_ac_online)
        *out_on_ac = false;
}

#endif /* __linux__ && !HU_IS_TEST */

int hu_scheduler_probe_load_pct(void) {
    int test_override = read_env_int("HU_TEST_LOAD_PCT", -2);
    if (test_override != -2)
        return test_override;
#if defined(__APPLE__) && !defined(HU_IS_TEST)
    return probe_load_macos();
#elif defined(__linux__) && !defined(HU_IS_TEST)
    return probe_load_linux();
#else
    return -1;
#endif
}

int hu_scheduler_probe_battery_pct(void) {
    int test_override = read_env_int("HU_TEST_BATTERY_PCT", -2);
    if (test_override != -2)
        return test_override;
#if defined(__APPLE__) && !defined(HU_IS_TEST)
    int b = -1;
    bool ac = true;
    probe_power_macos(&b, &ac);
    return b;
#elif defined(__linux__) && !defined(HU_IS_TEST)
    int b = -1;
    bool ac = true;
    probe_power_linux(&b, &ac);
    return b;
#else
    return -1;
#endif
}

bool hu_scheduler_probe_on_ac_power(void) {
    /* HU_TEST_ON_AC sentinel: when set in test mode use the explicit
     * value; otherwise consult the OS. Default true: when the host
     * can't tell, we'd rather run jobs than silently never schedule. */
#ifdef HU_IS_TEST
    const char *v = getenv("HU_TEST_ON_AC");
    if (v && *v) {
        if (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y')
            return true;
        if (v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N')
            return false;
    }
#endif
#if defined(__APPLE__) && !defined(HU_IS_TEST)
    int b = -1;
    bool ac = true;
    probe_power_macos(&b, &ac);
    return ac;
#elif defined(__linux__) && !defined(HU_IS_TEST)
    int b = -1;
    bool ac = true;
    probe_power_linux(&b, &ac);
    return ac;
#else
    return true;
#endif
}

bool hu_scheduler_probe_quiet_hours(int64_t now_ms, const hu_persona_t *persona) {
    /* Test override wins. */
#ifdef HU_IS_TEST
    const char *v = getenv("HU_TEST_QUIET_HOURS");
    if (v && *v) {
        if (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y')
            return true;
        if (v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N')
            return false;
    }
#endif
    /* Production fallback: when a persona is in scope, use a fixed
     * 01:00–06:00 quiet window — the sleep-compute sweet spot where
     * most personal devices are idle and on AC power. The persona
     * overlay strings will eventually provide per-user overrides; for
     * now a sane default beats "always available". A NULL persona
     * means "no quiet-hours opinion" → false. */
    (void)persona;
    if (now_ms <= 0)
        return false;
    if (!persona)
        return false;
    time_t t = (time_t)(now_ms / 1000);
    struct tm tm_local;
#if defined(_WIN32)
    if (localtime_s(&tm_local, &t) != 0)
        return false;
#else
    if (!localtime_r(&t, &tm_local))
        return false;
#endif
    int hour = tm_local.tm_hour;
    return (hour >= 1 && hour < 6);
}
