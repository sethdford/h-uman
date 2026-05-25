/* include/human/doctor/check_prompt_budget.h
 *
 * Sprint 55 B3 Task 5 — prompt budget doctor check.
 *
 * Reports the operator-facing state of the prompt-budget pipeline:
 *   - Is `prompt_budget.enabled` in config.json? (observation gate)
 *   - Is `prompt_budget_trim_enabled` in agent.prompt_budget config?
 *     (trim gate)
 *   - Threshold + sample-count config values
 *
 * Limitation (documented in detail_json): the doctor command runs
 * WITHOUT a live hu_agent_t, so it cannot report observation counts
 * or actual DEAD-field detections. Those require a running daemon
 * inspecting agent->prompt_budget directly. A future check (or
 * doctor-with-agent variant) can extend this with live stats.
 *
 * Verdict semantics:
 *   PASS — config has prompt_budget.enabled=true AND trim_enabled=true
 *   NA   — config has neither enabled (legacy path)
 *   FAIL — config is structurally invalid (e.g. trim_enabled=true but
 *          enabled=false → trim has no observation source)
 *
 * ctx contract: `const struct hu_config *` (same as provider_smoke).
 * NULL ctx → NA "no config given to doctor".
 */
#ifndef HU_DOCTOR_CHECK_PROMPT_BUDGET_H
#define HU_DOCTOR_CHECK_PROMPT_BUDGET_H

#include "human/core/error.h"
#include "human/doctor/check.h"

struct hu_config;

/* Sprint 55 B3 Task 5 — ctx contract for the prompt_budget check.
 *
 * The registry's adapter constructs this from its private state and
 * passes its address as the ctx argument. NULL cfg → check returns
 * NA with "no config provided" reason. */
typedef struct hu_doctor_check_prompt_budget_ctx {
    const struct hu_config *cfg;
} hu_doctor_check_prompt_budget_ctx_t;

/* Public vtable — registered by registry.c::register_defaults as the
 * 13th default check. */
extern hu_doctor_check_t hu_doctor_check_prompt_budget;

#endif /* HU_DOCTOR_CHECK_PROMPT_BUDGET_H */
