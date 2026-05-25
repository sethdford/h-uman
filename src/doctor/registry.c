#include "human/core/error.h"
#include "human/doctor.h"
#include "human/doctor/check.h"
#include <stdlib.h>
#include <string.h>

#define HU_DOCTOR_REGISTRY_INITIAL_CAP 16

typedef struct hu_doctor_registry {
    hu_allocator_t *alloc;
    hu_doctor_check_t *checks;
    size_t count;
    size_t cap;
} hu_doctor_registry_t;

hu_error_t hu_doctor_registry_init(hu_allocator_t *alloc, hu_doctor_registry_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_doctor_registry_t *r =
        (hu_doctor_registry_t *)alloc->alloc(alloc->ctx, sizeof(hu_doctor_registry_t));
    if (!r)
        return HU_ERR_OUT_OF_MEMORY;

    r->alloc = alloc;
    r->checks = (hu_doctor_check_t *)alloc->alloc(alloc->ctx, sizeof(hu_doctor_check_t) *
                                                                  HU_DOCTOR_REGISTRY_INITIAL_CAP);
    if (!r->checks) {
        alloc->free(alloc->ctx, r, sizeof(hu_doctor_registry_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    r->count = 0;
    r->cap = HU_DOCTOR_REGISTRY_INITIAL_CAP;

    *out = r;
    return HU_OK;
}

hu_error_t hu_doctor_registry_register(hu_doctor_registry_t *r, const hu_doctor_check_t *check) {
    if (!r || !check)
        return HU_ERR_INVALID_ARGUMENT;

    if (r->count >= r->cap) {
        size_t new_cap = r->cap * 2;
        hu_doctor_check_t *new_checks = (hu_doctor_check_t *)r->alloc->alloc(
            r->alloc->ctx, sizeof(hu_doctor_check_t) * new_cap);
        if (!new_checks)
            return HU_ERR_OUT_OF_MEMORY;

        memcpy(new_checks, r->checks, sizeof(hu_doctor_check_t) * r->count);
        r->alloc->free(r->alloc->ctx, r->checks, sizeof(hu_doctor_check_t) * r->cap);
        r->checks = new_checks;
        r->cap = new_cap;
    }

    memcpy(&r->checks[r->count], check, sizeof(hu_doctor_check_t));
    r->count++;
    return HU_OK;
}

hu_error_t hu_doctor_registry_run_all(hu_doctor_registry_t *r, void *ctx,
                                      hu_doctor_check_result_t *out_results, size_t *out_count,
                                      size_t cap) {
    if (!r || !out_results || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    if (cap < r->count)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < r->count; i++) {
        hu_doctor_check_t *check = &r->checks[i];
        if (check->run) {
            out_results[i] = check->run(check, ctx);
        } else {
            out_results[i] =
                (hu_doctor_check_result_t){HU_DOCTOR_FAIL, "check has no run function", NULL};
        }
    }

    *out_count = r->count;
    return HU_OK;
}

void hu_doctor_registry_free(hu_doctor_registry_t *r) {
    if (!r)
        return;
    if (r->checks) {
        r->alloc->free(r->alloc->ctx, r->checks, sizeof(hu_doctor_check_t) * r->cap);
    }
    r->alloc->free(r->alloc->ctx, r, sizeof(hu_doctor_registry_t));
}

/* ────────────────────────────────────────────────────────────────────
 * Default check registration
 * ──────────────────────────────────────────────────────────────────── */

/* Adapter functions: convert existing hu_doctor_check_* functions to
 * vtable entries. Each adapter wraps a legacy check function that returns
 * hu_error_t and appends to diag_item arrays. We adapt it to the
 * hu_doctor_check_result_t return value. */

typedef struct {
    hu_allocator_t *alloc;
    hu_diag_item_t *items;
    size_t count;
    size_t cap;
} hu_doctor_adapter_ctx_t;

/* Wrapper: install check */
static hu_doctor_check_result_t run_install_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_error_t err =
        hu_doctor_check_install(uctx->alloc, NULL, &uctx->items, &uctx->count, &uctx->cap);
    /* Map error to verdict */
    hu_doctor_verdict_t verdict = (err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL;
    return (hu_doctor_check_result_t){verdict, "", NULL};
}

/* Wrapper: config_semantics check */
static hu_doctor_check_result_t run_config_semantics_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_error_t err =
        hu_doctor_check_config_semantics(uctx->alloc, NULL, &uctx->items, &uctx->count);
    hu_doctor_verdict_t verdict = (err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL;
    return (hu_doctor_check_result_t){verdict, "", NULL};
}

/* Wrapper: security check */
static hu_doctor_check_result_t run_security_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_error_t err = hu_doctor_check_security(uctx->alloc, &uctx->items, &uctx->count, &uctx->cap);
    hu_doctor_verdict_t verdict = (err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL;
    return (hu_doctor_check_result_t){verdict, "", NULL};
}

/* Wrapper: memory_health check */
static hu_doctor_check_result_t run_memory_health_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_error_t err =
        hu_doctor_check_memory_health(uctx->alloc, NULL, &uctx->items, &uctx->count, &uctx->cap);
    hu_doctor_verdict_t verdict = (err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL;
    return (hu_doctor_check_result_t){verdict, "", NULL};
}

/* Wrapper: skills check */
static hu_doctor_check_result_t run_skills_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_error_t err = hu_doctor_check_skills(uctx->alloc, &uctx->items, &uctx->count, &uctx->cap);
    hu_doctor_verdict_t verdict = (err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL;
    return (hu_doctor_check_result_t){verdict, "", NULL};
}

/* Wrapper: imessage check */
static hu_doctor_check_result_t run_imessage_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    /* Use a stale_after_secs of 600 (10 minutes) by default */
    hu_error_t err =
        hu_doctor_check_imessage(uctx->alloc, 0, 600, &uctx->items, &uctx->count, &uctx->cap);
    hu_doctor_verdict_t verdict = (err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL;
    return (hu_doctor_check_result_t){verdict, "", NULL};
}

/* Wrapper: verifier check */
static hu_doctor_check_result_t run_verifier_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_error_t err =
        hu_doctor_check_verifier(uctx->alloc, 0, 600, 0.3, &uctx->items, &uctx->count, &uctx->cap);
    hu_doctor_verdict_t verdict = (err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL;
    return (hu_doctor_check_result_t){verdict, "", NULL};
}

/* Wrapper: scheduler check */
static hu_doctor_check_result_t run_scheduler_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_error_t err =
        hu_doctor_check_scheduler(uctx->alloc, 0, 600, &uctx->items, &uctx->count, &uctx->cap);
    hu_doctor_verdict_t verdict = (err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL;
    return (hu_doctor_check_result_t){verdict, "", NULL};
}

/* Wrapper: response_pipeline check */
static hu_doctor_check_result_t run_response_pipeline_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_error_t err =
        hu_doctor_check_response_pipeline(uctx->alloc, &uctx->items, &uctx->count, &uctx->cap);
    hu_doctor_verdict_t verdict = (err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL;
    return (hu_doctor_check_result_t){verdict, "", NULL};
}

/* Wrapper: inference check */
static hu_doctor_check_result_t run_inference_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_error_t err = hu_doctor_check_inference(uctx->alloc, &uctx->items, &uctx->count, &uctx->cap);
    hu_doctor_verdict_t verdict = (err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL;
    return (hu_doctor_check_result_t){verdict, "", NULL};
}

hu_error_t hu_doctor_registry_register_defaults(hu_doctor_registry_t *r) {
    if (!r)
        return HU_ERR_INVALID_ARGUMENT;

    /* Define check entries in registration order per architecture.md §2 */
    hu_doctor_check_t checks[] = {
        {"install", "Verifies binary and config layout", run_install_check, NULL, NULL},
        {"config_semantics", "Checks configuration semantics", run_config_semantics_check, NULL,
         NULL},
        {"security", "Validates security posture", run_security_check, NULL, NULL},
        {"memory_health", "Checks memory backend health", run_memory_health_check, NULL, NULL},
        {"skills", "Verifies skill registry", run_skills_check, NULL, NULL},
        {"imessage", "Diagnoses iMessage channel", run_imessage_check, NULL, NULL},
        {"verifier", "Checks response verifier health", run_verifier_check, NULL, NULL},
        {"scheduler", "Checks scheduler status", run_scheduler_check, NULL, NULL},
        {"response_pipeline", "Checks response pipeline", run_response_pipeline_check, NULL, NULL},
        {"inference", "Validates inference configuration", run_inference_check, NULL, NULL},
    };

    size_t num_checks = sizeof(checks) / sizeof(checks[0]);
    for (size_t i = 0; i < num_checks; i++) {
        hu_error_t err = hu_doctor_registry_register(r, &checks[i]);
        if (err != HU_OK)
            return err;
    }

    return HU_OK;
}
