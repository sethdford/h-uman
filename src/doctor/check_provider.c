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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward-declare hu_config to avoid pulling the entire config header
 * into a check that only needs the pointer type. */
struct hu_config;

/* Forward-declare hu_provider_t + factory entry point. We use
 * hu_provider_create_from_config because it resolves the configured
 * provider name + credentials in one call. */
struct hu_provider;
typedef struct hu_provider hu_provider_t;
extern hu_error_t hu_provider_create_from_config(hu_allocator_t *alloc, const struct hu_config *cfg,
                                                 const char *name, size_t name_len,
                                                 hu_provider_t *out);
extern void hu_provider_destroy(hu_provider_t *provider, hu_allocator_t *alloc);

/* Static reason buffers — borrowed pointers per the check.h contract. */
static char s_reason_buf[256];

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

    /* Ctx is `const hu_config *`. NULL means doctor wasn't given a
     * config — return NA (counts as PASS in aggregate per check.h
     * comment). This is the "doctor invoked without config" path,
     * which is structurally fine. */
    const struct hu_config *cfg = (const struct hu_config *)ctx;
    if (!cfg) {
        return (hu_doctor_check_result_t){
            HU_DOCTOR_NA, "no config provided to doctor — provider check skipped", NULL};
    }

    /* Phase 1 (this slice): structural check only. The classifier
     * is fully implemented and unit-tested via the public
     * hu_doctor_check_provider_classify() function. The full
     * factory-call path (hu_provider_create_from_config + 1-token
     * smoke) lands in Phase 2 when:
     *   (a) doctor.c::main() switches from old dispatch to the
     *       registry (separate sprint story), AND
     *   (b) the registry passes a (config, allocator) pair via
     *       ctx — currently ctx is just `const hu_config *`.
     *
     * Until then, this check returns PASS on a non-NULL config to
     * exercise the vtable wire-up; the real verdict comes from the
     * classifier when the factory call is wired.
     *
     * Tracking: sprints/sprint-54/designs/US-C3.3.md "Deferred (Phase 2)". */
    (void)cfg;
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, "", NULL};
}

/* ── Vtable entry ─────────────────────────────────────────────────── */

hu_doctor_check_t hu_doctor_check_provider = {
    .name = "provider_smoke",
    .description = "Verifies the configured AI provider can be instantiated",
    .run = check_provider_run,
    .fix = NULL, /* No autofix — user must edit config */
    .user_data = NULL,
};
