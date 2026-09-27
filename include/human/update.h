#ifndef HU_UPDATE_H
#define HU_UPDATE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

struct hu_config;

hu_error_t hu_update_check(char *version_buf, size_t buf_size);
hu_error_t hu_update_apply(void);

/* Decision for the periodic auto-check, derived from config.auto_update.
 * NULL, "" and "off" -> OFF (never checks, never applies). Exactly "apply"
 * -> APPLY. Anything else (including "check" and typos) -> CHECK, which only
 * prints a notice. Compiled in every build, including HU_IS_TEST, so the
 * off-never-applies gate is pinned by tests/test_update.c. */
typedef enum {
    HU_UPDATE_MODE_OFF = 0,
    HU_UPDATE_MODE_CHECK = 1,
    HU_UPDATE_MODE_APPLY = 2,
} hu_update_mode_t;

hu_update_mode_t hu_update_mode_from_config(const char *auto_update);

/* Semver comparison: returns <0 if a<b, 0 if a==b, >0 if a>b.
 * Parses "major.minor.patch" numerically. Leading 'v' is stripped. */
int hu_version_compare(const char *a, const char *b);

/* Periodic auto-check: reads ~/.human/.last_update_check timestamp,
 * checks staleness vs config interval, notifies or applies based on
 * config.auto_update ("off", "check", "apply"). */
hu_error_t hu_update_maybe_check(hu_allocator_t *alloc, const struct hu_config *cfg);

#endif /* HU_UPDATE_H */
