/* include/human/doctor/check_provider.h
 *
 * Sprint 54 US-C3.3 (Phase 1) — Provider smoke check.
 *
 * Validates that the configured AI provider can be instantiated. The
 * registered check plugs into the doctor registry from US-C3.1.
 *
 * ── ctx contract ──
 *
 * The registry's `void *ctx` IS, for this check, expected to be
 * `const struct hu_config *`. Cast accordingly. NULL is allowed and
 * means "no config available" → returns HU_DOCTOR_NA so the doctor
 * can report "no provider configured" without false-FAIL.
 *
 * ── deferred scope (Phase 2 follow-up) ──
 *
 * AC-1.2 of the design (1-token complete() returning <10s) is NOT
 * exercised in this slice. The check verifies provider can be
 * INSTANTIATED, not that it can ROUND-TRIP to the API. The smoke
 * call is gated by HU_IS_TEST in production and currently no-op'd
 * in test mode. Phase 2 wires the actual call when:
 *   - The doctor's main() uses the registry (currently still on the
 *     old dispatch path; that wire-up is a separate sprint story).
 *   - A mock-provider failure-injection pattern is established for
 *     the credential-invalid/rate-limited/unreachable test cases.
 *
 * Tracking: sprints/sprint-54/designs/US-C3.3.md "Out of scope".
 */
#ifndef HU_DOCTOR_CHECK_PROVIDER_H
#define HU_DOCTOR_CHECK_PROVIDER_H

#include "human/core/error.h"
#include "human/doctor/check.h"

/* Public vtable entry — registered by registry.c::register_defaults */
extern hu_doctor_check_t hu_doctor_check_provider;

/* Classification of why the smoke check returned FAIL.
 * Kept as opaque enum tag in detail_json for the --json output story
 * (US-C3.7) to consume; the reason string is human-readable. */
typedef enum hu_doctor_provider_reason {
    HU_DOCTOR_PROVIDER_OK = 0,
    HU_DOCTOR_PROVIDER_NOT_CONFIGURED,
    HU_DOCTOR_PROVIDER_CREDENTIALS_MISSING,
    HU_DOCTOR_PROVIDER_CREDENTIALS_INVALID,
    HU_DOCTOR_PROVIDER_RATE_LIMITED,
    HU_DOCTOR_PROVIDER_UNREACHABLE,
    HU_DOCTOR_PROVIDER_OTHER, /* unmapped error class */
} hu_doctor_provider_reason_t;

/* Test helper: classify a hu_error_t from hu_provider_create_from_config
 * into one of the reason variants. Exposed for direct unit testing
 * without needing to construct a real config struct.
 *
 * The mapping is:
 *   HU_OK                       → HU_DOCTOR_PROVIDER_OK
 *   HU_ERR_INVALID_ARGUMENT     → HU_DOCTOR_PROVIDER_NOT_CONFIGURED
 *   HU_ERR_PROVIDER_AUTH        → HU_DOCTOR_PROVIDER_CREDENTIALS_INVALID
 *   HU_ERR_PROVIDER_RATE_LIMITED→ HU_DOCTOR_PROVIDER_RATE_LIMITED
 *   HU_ERR_PROVIDER_UNAVAILABLE → HU_DOCTOR_PROVIDER_UNREACHABLE
 *   HU_ERR_CONFIG_NOT_FOUND     → HU_DOCTOR_PROVIDER_CREDENTIALS_MISSING
 *   anything else               → HU_DOCTOR_PROVIDER_OTHER
 *
 * Pure function; safe to call from any context without side effects. */
hu_doctor_provider_reason_t hu_doctor_check_provider_classify(hu_error_t err);

/* Test helper: map a reason enum to its stable kebab-case string for
 * detail_json output. Borrowed string; never NULL (returns
 * "unknown" for invalid enum values). */
const char *hu_doctor_check_provider_reason_str(hu_doctor_provider_reason_t r);

/* Test helper: render a human-readable diagnostic message for a
 * reason enum into the check's shared static buffer. Returns the
 * borrowed pointer (never NULL; empty string for OK). Phase 2 will
 * use this from the production path; Phase 1 exposes it for tests. */
const char *hu_doctor_check_provider_reason_message(hu_doctor_provider_reason_t r);

#endif /* HU_DOCTOR_CHECK_PROVIDER_H */
