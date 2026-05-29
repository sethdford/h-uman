#include "human/core/time.h"
#include "human/persona.h"
#include "human/persona/pacing.h"
#include "test_framework.h"
#include <stdint.h>

/* AC-7 contract: pacing enforces elapsed >= min_delay_ms * 1.2 across N
 * iterations. Uses SHORT delays in tests to keep wall-clock manageable. */
static void pacing_enforces_minimum_one_twentieth_floor(void) {
    hu_persona_t p = {0};
    p.min_reply_delay_ms = 50;      /* 50 * 1.2 = 60 ms floor */
    p.reply_delay_variance_ms = 20; /* ± 20 ms jitter on top */

    uint64_t start = 0;
    for (int i = 0; i < 5; i++) {
        hu_persona_pace_reply_start(&start);
        /* Simulate INSTANT work — no sleep, finish immediately. */
        hu_persona_pace_reply_finish(&p, start);
        uint64_t end = hu_time_get_current_ms();
        int64_t elapsed = (int64_t)(end - start);
        HU_ASSERT(elapsed >= 60); /* the 1.2x floor */
    }
}

/* AC: variance=0 means pacing converges to exactly min_delay*1.2. */
static void variance_zero_yields_deterministic_pacing(void) {
    hu_persona_t p = {0};
    p.min_reply_delay_ms = 30;
    p.reply_delay_variance_ms = 0;

    uint64_t start = 0;
    hu_persona_pace_reply_start(&start);
    hu_persona_pace_reply_finish(&p, start);
    uint64_t end = hu_time_get_current_ms();
    int64_t elapsed = (int64_t)(end - start);
    HU_ASSERT(elapsed >= 36); /* 30 * 1.2 = 36 — the load-bearing contract */
    /* Upper bound is a generous "didn't go wildly over" sanity guard, NOT
     * the contract under test. A real over-pacing bug (wrong units, applying
     * a multi-second delay) blows past 2000ms; ordinary CI-runner scheduler
     * jitter does not. Keep it loose so the floor assertion above is what
     * pins behavior, and the test stays deterministic under load. */
    HU_ASSERT(elapsed < 2000);
}

/* AC: min_delay=0 means pacing is a no-op (no sleep). */
static void zero_min_delay_disables_pacing(void) {
    hu_persona_t p = {0};
    p.min_reply_delay_ms = 0;
    p.reply_delay_variance_ms = 100;

    uint64_t start = 0;
    hu_persona_pace_reply_start(&start);
    hu_persona_pace_reply_finish(&p, start);
    uint64_t end = hu_time_get_current_ms();
    int64_t elapsed = (int64_t)(end - start);
    /* min_delay=0 means the no-op path must NOT apply the variance delay.
     * A bug that wrongly paces would sleep on the order of the 100ms
     * variance; an 80ms ceiling cleanly separates "no pacing applied"
     * (jitter only) from "wrongly slept" (>=100ms) without flaking on
     * loaded CI runners. */
    HU_ASSERT(elapsed < 80);
}

void run_imessage_reply_pacing_tests(void) {
    HU_TEST_SUITE("imessage_reply_pacing");
    HU_RUN_TEST(pacing_enforces_minimum_one_twentieth_floor);
    HU_RUN_TEST(variance_zero_yields_deterministic_pacing);
    HU_RUN_TEST(zero_min_delay_disables_pacing);
}
