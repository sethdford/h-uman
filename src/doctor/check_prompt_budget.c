/* src/doctor/check_prompt_budget.c
 *
 * Sprint 55 B3 Task 5 — prompt budget doctor check implementation.
 *
 * See include/human/doctor/check_prompt_budget.h for the contract,
 * verdict semantics, and the documented limitation that this check
 * inspects CONFIG STATE (not a live agent's observations).
 */

#include "human/doctor/check_prompt_budget.h"

#include "human/config.h"
#include "human/doctor/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Static buffers for borrowed strings per the check.h contract.
 * Sized for the longest message + the JSON detail. */
static char s_reason_buf[256];
static char s_detail_json_buf[512];

/* ── vtable runner ────────────────────────────────────────────────── */

static hu_doctor_check_result_t check_prompt_budget_run(hu_doctor_check_t *self, void *ctx) {
    (void)self;

    /* ctx is hu_doctor_check_prompt_budget_ctx_t (constructed by
     * registry.c::run_prompt_budget_check from the adapter). NULL ctx
     * OR NULL cfg → NA. */
    const hu_doctor_check_prompt_budget_ctx_t *pctx =
        (const hu_doctor_check_prompt_budget_ctx_t *)ctx;
    const struct hu_config *cfg = pctx ? pctx->cfg : NULL;

    if (!cfg) {
        return (hu_doctor_check_result_t){HU_DOCTOR_NA,
                                          "no config provided to doctor — prompt_budget check "
                                          "skipped",
                                          NULL};
    }

    bool enabled = cfg->prompt_budget.enabled;
    /* The trim gate also has per-prompt-config flags that aren't on
     * hu_config_t — they live on hu_prompt_config_t per-call. We can
     * only inspect the runtime-config side here. */

    /* detail_json — minimal schema so operators + --json consumers can
     * parse it. Documents the doctor-scope limitation explicitly. */
    snprintf(s_detail_json_buf, sizeof(s_detail_json_buf),
             "{\"enabled\":%s,"
             "\"observation_count\":null,"
             "\"dead_field_count\":null,"
             "\"note\":\"doctor runs without a live agent; observation_count + "
             "dead_field_count require a running daemon inspecting agent->prompt_budget. "
             "This check reports CONFIG STATE only.\"}",
             enabled ? "true" : "false");

    if (!enabled) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "prompt_budget.enabled=false in config.json — observer + trim gate "
                 "inactive. Set true to activate.");
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason_buf, s_detail_json_buf};
    }

    /* enabled=true → reportPASS with the detail_json carrying the
     * observation-count caveat. We're honest: the doctor cannot
     * verify the trim is actually firing. */
    snprintf(s_reason_buf, sizeof(s_reason_buf),
             "prompt_budget.enabled=true in config.json — observer wired in agent init. "
             "Use logs to confirm trim gate is firing.");
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason_buf, s_detail_json_buf};
}

/* ── Vtable entry ─────────────────────────────────────────────────── */

hu_doctor_check_t hu_doctor_check_prompt_budget = {
    .name = "prompt_budget",
    .description = "Reports prompt-budget config state (observer + trim gate)",
    .run = check_prompt_budget_run,
    .fix = NULL, /* No autofix — operator edits config.json */
    .user_data = NULL,
};
