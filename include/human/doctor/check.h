#ifndef HU_DOCTOR_CHECK_H
#define HU_DOCTOR_CHECK_H

#include "human/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum hu_doctor_verdict {
    HU_DOCTOR_PASS = 0,
    HU_DOCTOR_FAIL = 1,
    HU_DOCTOR_NA = 2, /* platform-not-applicable; counts as PASS in aggregate */
} hu_doctor_verdict_t;

typedef struct hu_doctor_check_result {
    hu_doctor_verdict_t verdict;
    /* Borrowed string — points into a per-check static or a buffer the
     * check owns for the lifetime of the call. Registry never frees this. */
    const char *reason;
    /* Optional structured detail for --json. NULL = no detail. */
    const char *detail_json;
} hu_doctor_check_result_t;

typedef struct hu_doctor_check {
    const char *name;        /* stable identifier — used in --json + exit-code tests */
    const char *description; /* one-line human-readable */
    /* Run the check. ctx is registry-provided (config, allocator, ...). */
    hu_doctor_check_result_t (*run)(struct hu_doctor_check *self, void *ctx);
    /* OPTIONAL — NULL means "no autofix available." Returns true if fix
     * was applied (so the registry can re-run the check). */
    bool (*fix)(struct hu_doctor_check *self, void *ctx, bool interactive);
    /* Per-check user data (cast to whatever the check stores). */
    void *user_data;
} hu_doctor_check_t;

typedef struct hu_doctor_registry hu_doctor_registry_t;

hu_error_t hu_doctor_registry_init(hu_allocator_t *alloc, hu_doctor_registry_t **out);

hu_error_t hu_doctor_registry_register(hu_doctor_registry_t *r, const hu_doctor_check_t *check);

/* Run every check sequentially in registration order. Writes
 * out_results[i] for each registered check. Returns OK even if some
 * checks FAILed — the aggregate verdict comes from the caller
 * inspecting out_results. */
hu_error_t hu_doctor_registry_run_all(hu_doctor_registry_t *r, void *ctx,
                                      hu_doctor_check_result_t *out_results, size_t *out_count,
                                      size_t cap);

void hu_doctor_registry_free(hu_doctor_registry_t *r);

/* Helper: register all default checks into the registry */
hu_error_t hu_doctor_registry_register_defaults(hu_doctor_registry_t *r);

#endif /* HU_DOCTOR_CHECK_H */
