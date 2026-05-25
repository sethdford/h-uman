/* src/doctor/exit_code.c
 *
 * Sprint 54 US-C3.9 (Phase 1) — Doctor exit-code aggregator.
 *
 * Pure function that maps a list of per-check results to one of the
 * HU_DOCTOR_EXIT_* constants defined in include/human/doctor.h.
 *
 * Phase 1 scope:
 *   - The function exists, is pure (no I/O), and is fully tested.
 *   - The constants are documented in docs/guides/doctor.md.
 *   - A pre-commit parity script asserts docs and src/main.c (when it
 *     starts calling exit() with these) stay aligned.
 *
 * Phase 2 (deferred):
 *   - cmd_doctor() in src/main.c invokes this function and exits with
 *     the result. Requires the registry-driven main() rewrite which is
 *     a separate sprint story.
 *   - Crash detection via atexit handler for code 64. Phase 1 defines
 *     the constant; Phase 2 wires the handler.
 */

#include "human/doctor.h"
#include "human/doctor/check.h"

#include <stddef.h>
#include <string.h>

/* Inspect a single check's detail_json for the bug-grade marker.
 *
 * The contract: a check is bug-grade iff its detail_json string contains
 * the substring `"category":"bug"` (with allowance for whitespace
 * around the colon). The substring search is intentionally simple —
 * detail_json is small JSON, and we don't want to drag a parser into
 * this exit-code path. A check that wants to be bug-grade must emit
 * exactly this key/value; misspellings default to user-action.
 *
 * Returns true iff bug-grade. NULL or empty detail_json → false. */
static bool result_is_bug_grade(const struct hu_doctor_check_result *r) {
    if (!r || !r->detail_json || r->detail_json[0] == '\0')
        return false;
    /* Permissive whitespace match: `"category":"bug"` or `"category" :
     * "bug"`. Keep simple — strstr on the canonical form covers the
     * 95% case; the parity script enforces the contract at write time. */
    if (strstr(r->detail_json, "\"category\":\"bug\""))
        return true;
    if (strstr(r->detail_json, "\"category\": \"bug\""))
        return true;
    if (strstr(r->detail_json, "\"category\" : \"bug\""))
        return true;
    return false;
}

int hu_doctor_compute_exit_code(const struct hu_doctor_check_result *results, size_t count) {
    /* No results to inspect → OK. A doctor invocation with zero
     * registered checks is structurally fine (nothing to fail). */
    if (!results || count == 0)
        return HU_DOCTOR_EXIT_OK;

    bool any_bug_grade = false;
    bool any_user_action = false;

    for (size_t i = 0; i < count; i++) {
        switch (results[i].verdict) {
        case HU_DOCTOR_PASS:
        case HU_DOCTOR_NA:
            /* PASS and NA both count as OK; NA is "platform-not-applicable"
             * per the check.h contract — explicitly not a failure. */
            break;
        case HU_DOCTOR_FAIL:
            if (result_is_bug_grade(&results[i])) {
                any_bug_grade = true;
            } else {
                any_user_action = true;
            }
            break;
        default:
            /* Unknown verdict value (data corruption / future enum
             * variant we don't recognize) → bug-grade, the conservative
             * choice that flags the situation for a developer. */
            any_bug_grade = true;
            break;
        }
    }

    /* Bug-grade trumps user-action when both are present. The user
     * sees both in the reason output; the exit code reflects the
     * higher severity so an alerting integration triggers the right
     * runbook. */
    if (any_bug_grade)
        return HU_DOCTOR_EXIT_BUG_GRADE;
    if (any_user_action)
        return HU_DOCTOR_EXIT_USER_ACTION;
    return HU_DOCTOR_EXIT_OK;
}
