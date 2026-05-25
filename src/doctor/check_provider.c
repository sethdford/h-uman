/* src/doctor/check_provider.c
 *
 * Sprint 54 US-C3.3 (Phase 1) — Provider smoke check implementation.
 *
 * See include/human/doctor/check_provider.h for the public contract,
 * the ctx interpretation (const hu_config *), and the deferred-scope
 * caveat on AC-1.2 (no 1-token call yet; instantiation only).
 *
 * Structure:
 *   - hu_doctor_check_provider_classify: pure error-code mapper
 *   - hu_doctor_check_provider_reason_str: stable kebab-case strings
 *   - check_provider_run: vtable runner (calls hu_provider_create_from_config)
 *   - hu_doctor_check_provider: vtable entry
 */

#include "human/doctor/check_provider.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor/check.h"
#include "human/providers/factory.h" /* pulls in hu_provider_t */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Static reason buffers — borrowed pointers per the check.h contract. */
static char s_reason_buf[256];

#ifdef HU_IS_TEST
/* Sprint 55 Phase 3 — fault-injection state. See header for usage. */
static bool s_inject_err_active = false;
static hu_error_t s_inject_err_value = HU_OK;

void hu_doctor_check_provider_inject_error_for_test(hu_error_t err) {
    s_inject_err_active = true;
    s_inject_err_value = err;
}

void hu_doctor_check_provider_inject_error_for_test_reset(void) {
    s_inject_err_active = false;
    s_inject_err_value = HU_OK;
}
#endif

/* ── Pure helpers (testable in isolation) ─────────────────────────── */

hu_doctor_provider_reason_t hu_doctor_check_provider_classify(hu_error_t err) {
    switch (err) {
    case HU_OK:
        return HU_DOCTOR_PROVIDER_OK;
    case HU_ERR_INVALID_ARGUMENT:
        return HU_DOCTOR_PROVIDER_NOT_CONFIGURED;
    case HU_ERR_CONFIG_NOT_FOUND:
    case HU_ERR_NOT_FOUND:
        return HU_DOCTOR_PROVIDER_CREDENTIALS_MISSING;
    case HU_ERR_PROVIDER_AUTH:
        return HU_DOCTOR_PROVIDER_CREDENTIALS_INVALID;
    case HU_ERR_PROVIDER_RATE_LIMITED:
        return HU_DOCTOR_PROVIDER_RATE_LIMITED;
    case HU_ERR_PROVIDER_UNAVAILABLE:
        return HU_DOCTOR_PROVIDER_UNREACHABLE;
    default:
        return HU_DOCTOR_PROVIDER_OTHER;
    }
}

const char *hu_doctor_check_provider_reason_str(hu_doctor_provider_reason_t r) {
    switch (r) {
    case HU_DOCTOR_PROVIDER_OK:
        return "ok";
    case HU_DOCTOR_PROVIDER_NOT_CONFIGURED:
        return "not-configured";
    case HU_DOCTOR_PROVIDER_CREDENTIALS_MISSING:
        return "credentials-missing";
    case HU_DOCTOR_PROVIDER_CREDENTIALS_INVALID:
        return "credentials-invalid";
    case HU_DOCTOR_PROVIDER_RATE_LIMITED:
        return "rate-limited";
    case HU_DOCTOR_PROVIDER_UNREACHABLE:
        return "unreachable";
    case HU_DOCTOR_PROVIDER_OTHER:
        return "other";
    default:
        return "unknown";
    }
}

/* Human-readable rendering of the reason for the check's `reason`
 * field. Writes into the shared static buffer. Exposed via the
 * header for Phase 1 test coverage; Phase 2 will use it from the
 * production run() path. */
const char *hu_doctor_check_provider_reason_message(hu_doctor_provider_reason_t r) {
    const char *kebab = hu_doctor_check_provider_reason_str(r);
    switch (r) {
    case HU_DOCTOR_PROVIDER_NOT_CONFIGURED:
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "no provider configured — add 'provider' to ~/.human/config.json");
        break;
    case HU_DOCTOR_PROVIDER_CREDENTIALS_MISSING:
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "credentials missing for configured provider — set the api_key");
        break;
    case HU_DOCTOR_PROVIDER_CREDENTIALS_INVALID:
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "credentials invalid (auth rejected by provider)");
        break;
    case HU_DOCTOR_PROVIDER_RATE_LIMITED:
        snprintf(s_reason_buf, sizeof(s_reason_buf), "provider rate-limited — retry after backoff");
        break;
    case HU_DOCTOR_PROVIDER_UNREACHABLE:
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "provider unreachable — check network connectivity");
        break;
    case HU_DOCTOR_PROVIDER_OTHER:
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "provider check failed (unmapped error class: %s)", kebab);
        break;
    case HU_DOCTOR_PROVIDER_OK:
    default:
        s_reason_buf[0] = '\0';
        break;
    }
    return s_reason_buf;
}

/* ── vtable runner ────────────────────────────────────────────────── */

static hu_doctor_check_result_t check_provider_run(hu_doctor_check_t *self, void *ctx) {
    (void)self;

    /* Sprint 55 Phase 2 — ctx is hu_doctor_check_provider_ctx_t *
     * (allocator + cfg). NULL ctx OR NULL cfg means doctor wasn't
     * given a config — return NA. */
    const hu_doctor_check_provider_ctx_t *pctx = (const hu_doctor_check_provider_ctx_t *)ctx;
    if (!pctx || !pctx->cfg) {
        return (hu_doctor_check_result_t){
            HU_DOCTOR_NA, "no config provided to doctor — provider check skipped", NULL};
    }
    if (!pctx->alloc) {
        /* Structural bug — registry must always pass an allocator. */
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL,
                                          "provider check invoked without allocator (bug)", NULL};
    }

#if HU_IS_TEST
    /* Sprint 55 Phase 3 — fault injection. If a test has called
     * hu_doctor_check_provider_inject_error_for_test, flow that error
     * through the same classify → message → verdict pipeline that the
     * production path uses. This lets tests pin every fault-mode
     * mapping end-to-end without spinning up a mock provider. */
    if (s_inject_err_active) {
        hu_error_t err = s_inject_err_value;
        hu_doctor_provider_reason_t reason = hu_doctor_check_provider_classify(err);
        if (err == HU_OK) {
            return (hu_doctor_check_result_t){HU_DOCTOR_PASS, "", NULL};
        }
        const char *msg = hu_doctor_check_provider_reason_message(reason);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, msg, NULL};
    }
    /* No injection — tests don't exercise real provider creation.
     * Return NA so the doctor run-all flow can be exercised end-to-
     * end without spinning up provider state. */
    return (hu_doctor_check_result_t){HU_DOCTOR_NA, "smoke check skipped under HU_IS_TEST", NULL};
#else
    /* Production path — call the configured provider's factory. We
     * pass name=NULL/0 so hu_provider_create_from_config uses
     * cfg->default_provider. The factory does NOT make a network
     * round-trip; it constructs the in-process vtable instance.
     * That's deliberately scoped: AC-1.2 of US-C3.3 asked only for
     * "can be instantiated", and going further (1-token API call)
     * requires a mock-provider fault-injection pattern we don't
     * have yet. The classifier is still the right shape for that
     * future extension. */
    hu_provider_t prov = (hu_provider_t){0};
    hu_error_t err = hu_provider_create_from_config(pctx->alloc, pctx->cfg, NULL, 0, &prov);
    hu_doctor_provider_reason_t reason = hu_doctor_check_provider_classify(err);

    if (err == HU_OK) {
        if (prov.vtable && prov.vtable->deinit)
            prov.vtable->deinit(prov.ctx, pctx->alloc);
        return (hu_doctor_check_result_t){HU_DOCTOR_PASS, "", NULL};
    }
    const char *msg = hu_doctor_check_provider_reason_message(reason);
    return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, msg, NULL};
#endif
}

/* ── Vtable entry ─────────────────────────────────────────────────── */

hu_doctor_check_t hu_doctor_check_provider = {
    .name = "provider_smoke",
    .description = "Verifies the configured AI provider can be instantiated",
    .run = check_provider_run,
    .fix = NULL, /* No autofix — user must edit config */
    .user_data = NULL,
};
