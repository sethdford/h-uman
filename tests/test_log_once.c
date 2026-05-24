#include "human/core/log.h"
#include "human/observer.h"
#include "test_framework.h"

#include <stdatomic.h>
#include <string.h>

/*
 * Tiny event-counter observer used to assert that hu_log_info_once /
 * hu_log_warn_once emit exactly one event per guard, regardless of
 * how many times the call site invokes them. Test contract is the
 * SHAPE the silent-config-gated-subsystems rule expects: one log
 * line on disable, even if the tick fires 1000 times.
 */
typedef struct {
    int count;
    char last_component[64];
    char last_message[256];
} log_once_capture_t;

static void capture_record_event(void *ctx, const hu_observer_event_t *event) {
    log_once_capture_t *cap = (log_once_capture_t *)ctx;
    cap->count++;
    if (event->tag == HU_OBSERVER_EVENT_ERR) {
        if (event->data.err.component) {
            strncpy(cap->last_component, event->data.err.component,
                    sizeof(cap->last_component) - 1);
            cap->last_component[sizeof(cap->last_component) - 1] = '\0';
        }
        if (event->data.err.message) {
            strncpy(cap->last_message, event->data.err.message, sizeof(cap->last_message) - 1);
            cap->last_message[sizeof(cap->last_message) - 1] = '\0';
        }
    }
}

static const hu_observer_vtable_t capture_vtable = {
    .record_event = capture_record_event,
    .record_metric = NULL,
    .flush = NULL,
    .name = NULL,
    .deinit = NULL,
};

static void test_log_info_once_emits_exactly_once_for_repeated_calls(void) {
    log_once_capture_t cap = {0};
    hu_observer_t obs = {.ctx = &cap, .vtable = &capture_vtable};
    atomic_bool guard = false;

    for (int i = 0; i < 100; i++) {
        hu_log_info_once(&guard, "test", &obs, "should fire once: %d", i);
    }

    HU_ASSERT_EQ(cap.count, 1);
    HU_ASSERT_STR_EQ(cap.last_component, "test");
    HU_ASSERT_TRUE(strstr(cap.last_message, "should fire once: 0") != NULL);
}

static void test_log_warn_once_independent_guards_fire_independently(void) {
    log_once_capture_t cap = {0};
    hu_observer_t obs = {.ctx = &cap, .vtable = &capture_vtable};
    atomic_bool guard_a = false;
    atomic_bool guard_b = false;

    hu_log_warn_once(&guard_a, "alpha", &obs, "alpha disabled");
    hu_log_warn_once(&guard_a, "alpha", &obs, "alpha disabled again");
    hu_log_warn_once(&guard_b, "beta", &obs, "beta disabled");
    hu_log_warn_once(&guard_b, "beta", &obs, "beta disabled again");

    HU_ASSERT_EQ(cap.count, 2);
}

static void test_log_info_once_with_null_observer_still_flips_guard(void) {
    /* NULL observer falls through to fprintf(stderr). The guard must still
     * flip so the next call is also suppressed — otherwise a daemon with no
     * observer attached would spam stderr every tick. */
    atomic_bool guard = false;

    /* First call: guard flips, would print to stderr (we don't capture). */
    hu_log_info_once(&guard, "noobs", NULL, "first");

    /* Second call: guard already true, no log_impl call. We verify by
     * checking the guard itself — it should remain true. */
    HU_ASSERT_TRUE(atomic_load(&guard));

    /* Calling again with a recording observer also should not fire because
     * the guard is already true. */
    log_once_capture_t cap = {0};
    hu_observer_t obs = {.ctx = &cap, .vtable = &capture_vtable};
    hu_log_info_once(&guard, "noobs", &obs, "should-not-fire");
    HU_ASSERT_EQ(cap.count, 0);
}

static void test_log_info_once_resetting_guard_re_arms_emission(void) {
    /* Test-only contract: callers that need to re-arm (e.g. in unit tests
     * with a per-test fresh log capture) can reset the guard to false. */
    log_once_capture_t cap = {0};
    hu_observer_t obs = {.ctx = &cap, .vtable = &capture_vtable};
    atomic_bool guard = false;

    hu_log_info_once(&guard, "rearm", &obs, "first");
    hu_log_info_once(&guard, "rearm", &obs, "suppressed");
    atomic_store(&guard, false);
    hu_log_info_once(&guard, "rearm", &obs, "second");

    HU_ASSERT_EQ(cap.count, 2);
}

void run_log_once_tests(void) {
    HU_TEST_SUITE("log_once");
    HU_RUN_TEST(test_log_info_once_emits_exactly_once_for_repeated_calls);
    HU_RUN_TEST(test_log_warn_once_independent_guards_fire_independently);
    HU_RUN_TEST(test_log_info_once_with_null_observer_still_flips_guard);
    HU_RUN_TEST(test_log_info_once_resetting_guard_re_arms_emission);
}
