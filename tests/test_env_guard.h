/*
 * tests/test_env_guard.h — runner-side HOME / HU_STATE_DIR isolation for tests
 * that drive a full hu_agent_turn in a scratch directory.
 *
 * A failed HU_ASSERT longjmps out of the test body, past its own cleanup, so a
 * test that points HOME at its tmpdir would leave it there for every later
 * suite. The save/restore therefore lives in the RUNNER, around HU_RUN_TEST
 * (same shape as tests/test_proactive.c's gap-7 wrapper): it runs whether the
 * test passed or longjmp'd. `scratch` is the test file's static buffer that
 * the test fills with its tmpdir path; the runner removes that dir.
 *
 * Header-only static inline, like test_tmpdir.h.
 */
#ifndef HU_TEST_ENV_GUARD_H
#define HU_TEST_ENV_GUARD_H

#include "test_framework.h"
#include "test_tmpdir.h"
#include <stdlib.h>
#include <string.h>

#define HU_TEST_ENV_GUARD_N 3

typedef struct {
    char *saved[HU_TEST_ENV_GUARD_N]; /* strdup'd previous values; NULL = was unset */
} hu_test_env_guard_t;

static const char *const hu__test_env_guard_vars[HU_TEST_ENV_GUARD_N] = {"HOME", "HU_STATE_DIR",
                                                                         "HU_HARD_MOMENT"};

static inline void hu_test_env_guard_save(hu_test_env_guard_t *g) {
    for (size_t i = 0; i < HU_TEST_ENV_GUARD_N; i++) {
        const char *v = getenv(hu__test_env_guard_vars[i]);
        g->saved[i] = v ? strdup(v) : NULL;
    }
}

static inline void hu_test_env_guard_restore(hu_test_env_guard_t *g) {
    for (size_t i = 0; i < HU_TEST_ENV_GUARD_N; i++) {
        if (g->saved[i]) {
            setenv(hu__test_env_guard_vars[i], g->saved[i], 1);
            free(g->saved[i]);
            g->saved[i] = NULL;
        } else {
            unsetenv(hu__test_env_guard_vars[i]);
        }
    }
}

/* HU_RUN_TEST with the env restored and `scratch` (a char array the test
 * fills with its tmpdir) removed afterwards, pass or fail. */
#define HU_RUN_TEST_ENV_GUARDED(fn, scratch) \
    do {                                     \
        hu_test_env_guard_t hu__eg;          \
        hu_test_env_guard_save(&hu__eg);     \
        HU_RUN_TEST(fn);                     \
        hu_test_env_guard_restore(&hu__eg);  \
        if ((scratch)[0]) {                  \
            hu_test_rm_rf(scratch);          \
            (scratch)[0] = '\0';             \
        }                                    \
    } while (0)

#endif /* HU_TEST_ENV_GUARD_H */
