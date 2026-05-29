#include "human/core/rand.h"
#include "test_framework.h"

#include <stdbool.h>
#include <stdint.h>

/* hu_rand_uniform is the portable non-cryptographic uniform-random helper
 * used for jitter/selection. It must behave identically across the BSD/macOS
 * arc4random path and the Linux getrandom/urandom + rejection-sampling path.
 * These tests pin the deterministic invariants (the only properties that can
 * be asserted without a fixed seed). */

static void test_rand_uniform_zero_bound_returns_zero(void) {
    /* bound 0 has no value in range; contract says return 0. */
    HU_ASSERT_EQ((int)hu_rand_uniform(0), 0);
}

static void test_rand_uniform_one_bound_returns_zero(void) {
    /* bound 1 → the only value in [0,1) is 0. */
    HU_ASSERT_EQ((int)hu_rand_uniform(1), 0);
}

static void test_rand_uniform_result_below_bound(void) {
    /* For a range of bounds, every draw must satisfy result < bound. Many
     * iterations exercise the rejection-sampling loop on the Linux path. */
    const uint32_t bounds[] = {2, 3, 7, 100, 1801, 8001, 90001};
    for (size_t b = 0; b < sizeof(bounds) / sizeof(bounds[0]); b++) {
        uint32_t bound = bounds[b];
        for (int i = 0; i < 2000; i++) {
            uint32_t r = hu_rand_uniform(bound);
            HU_ASSERT_TRUE(r < bound);
        }
    }
}

static void test_rand_uniform_full_range_observed(void) {
    /* With bound=2 and many draws, both 0 and 1 must appear — confirms the
     * generator is not stuck on a constant. Probability of a false failure
     * after 1000 draws is 2 * 2^-1000, i.e. effectively zero. */
    bool saw_zero = false;
    bool saw_one = false;
    for (int i = 0; i < 1000 && !(saw_zero && saw_one); i++) {
        uint32_t r = hu_rand_uniform(2);
        if (r == 0)
            saw_zero = true;
        else if (r == 1)
            saw_one = true;
    }
    HU_ASSERT_TRUE(saw_zero);
    HU_ASSERT_TRUE(saw_one);
}

void run_rand_tests(void) {
    HU_TEST_SUITE("rand");
    HU_RUN_TEST(test_rand_uniform_zero_bound_returns_zero);
    HU_RUN_TEST(test_rand_uniform_one_bound_returns_zero);
    HU_RUN_TEST(test_rand_uniform_result_below_bound);
    HU_RUN_TEST(test_rand_uniform_full_range_observed);
}
