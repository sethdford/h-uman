#include "seaclaw/memory/vector/circuit_breaker.h"
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static int64_t nano_ts(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return (int64_t)(uli.QuadPart * 100);
}
#else
#include <sys/time.h>
static int64_t nano_ts(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000000LL + (int64_t)tv.tv_usec * 1000LL;
}
#endif

void sc_circuit_breaker_init(sc_circuit_breaker_t *cb, uint32_t threshold, uint32_t cooldown_ms) {
    cb->state = SC_CB_CLOSED;
    cb->failure_count = 0;
    cb->threshold = threshold;
    cb->cooldown_ns = (uint64_t)cooldown_ms * 1000000;
    cb->last_failure_ns = 0;
    cb->half_open_probe_sent = false;
}

bool sc_circuit_breaker_allow(sc_circuit_breaker_t *cb) {
    switch (cb->state) {
    case SC_CB_CLOSED:
        return true;
    case SC_CB_OPEN: {
        int64_t now = nano_ts();
        if (now - cb->last_failure_ns >= (int64_t)cb->cooldown_ns) {
            cb->state = SC_CB_HALF_OPEN;
            cb->half_open_probe_sent = true;
            return true;
        }
        return false;
    }
    case SC_CB_HALF_OPEN:
        if (!cb->half_open_probe_sent) {
            cb->half_open_probe_sent = true;
            return true;
        }
        return false;
    }
    return false;
}

void sc_circuit_breaker_record_success(sc_circuit_breaker_t *cb) {
    cb->state = SC_CB_CLOSED;
    cb->failure_count = 0;
    cb->half_open_probe_sent = false;
}

void sc_circuit_breaker_record_failure(sc_circuit_breaker_t *cb) {
    if (cb->failure_count < 0xFFFFFFFFu)
        cb->failure_count++;
    cb->last_failure_ns = nano_ts();

    if (cb->state == SC_CB_HALF_OPEN ||
        (cb->state == SC_CB_CLOSED && cb->failure_count >= cb->threshold)) {
        cb->state = SC_CB_OPEN;
        cb->half_open_probe_sent = false;
    }
}

bool sc_circuit_breaker_is_open(const sc_circuit_breaker_t *cb) {
    return cb->state == SC_CB_OPEN;
}
