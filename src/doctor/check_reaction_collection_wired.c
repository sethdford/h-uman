/* src/doctor/check_reaction_collection_wired.c
 *
 * 2026-05 audit follow-up — see the header for full context.
 *
 * This .c file is itself the build-time observer: the `#ifdef
 * HU_ENABLE_RL_FULL` check below is evaluated at COMPILE time of THIS
 * translation unit, so the verdict baked into the binary reflects the
 * exact CMake flags used to build that binary. The operator running
 * `human doctor` therefore gets ground-truth about the running binary,
 * not just about what's documented to be possible. */

#include "human/doctor/check_reaction_collection_wired.h"

#include "human/config.h"
#include "human/doctor/check.h"

#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_RL_FULL
#define HU_BUILT_WITH_RL_FULL 1
#else
#define HU_BUILT_WITH_RL_FULL 0
#endif

/* Static reason + detail buffers — same lifetime contract as the
 * sibling check_prompt_budget. Doctor runs single-threaded; static is
 * safe because the result is consumed by the emitter before the next
 * invocation. */
static char s_reason_buf[512];
static char s_detail_json_buf[512];

/* Pure verdict logic — takes both facts explicitly so the public vtable
 * runner and the test seam share one code path. Buffers are caller-
 * provided so the test seam can hand in its own (avoiding race on the
 * static buffers when tests run interleaved with the production check). */
static hu_doctor_check_result_t derive(const struct hu_config *cfg, bool built_with_rl_full,
                                       char *reason_buf, size_t reason_cap, char *detail_buf,
                                       size_t detail_cap) {
    if (!cfg) {
        return (hu_doctor_check_result_t){
            HU_DOCTOR_NA, "no config provided to doctor — reaction_collection_wired check skipped",
            NULL};
    }

    bool cfg_enabled = cfg->reaction_collection.enabled;

    snprintf(detail_buf, detail_cap,
             "{\"reaction_collection_enabled\":%s,\"built_with_rl_full\":%s}",
             cfg_enabled ? "true" : "false", built_with_rl_full ? "true" : "false");

    if (!cfg_enabled) {
        return (hu_doctor_check_result_t){
            HU_DOCTOR_NA,
            "reaction_collection.enabled=false in config — DPO recorder not expected to run",
            detail_buf};
    }

    if (!built_with_rl_full) {
        snprintf(reason_buf, reason_cap,
                 "reaction_collection.enabled=true in config but binary was built "
                 "WITHOUT HU_ENABLE_RL_FULL — DPO recorder is compiled out and no "
                 "imessage_tapback pairs will ever be recorded. Rebuild with: "
                 "cmake -B build -DHU_ENABLE_RL_FULL=ON (or use the 'dev' preset) "
                 "and restart the daemon.");
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, reason_buf, detail_buf};
    }

    snprintf(reason_buf, reason_cap,
             "reaction_collection.enabled=true AND binary built with HU_ENABLE_RL_FULL "
             "— DPO recorder is wired and will fire on outbound + tapback pairs.");
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, reason_buf, detail_buf};
}

static hu_doctor_check_result_t check_run(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    const hu_doctor_check_reaction_collection_wired_ctx_t *pctx =
        (const hu_doctor_check_reaction_collection_wired_ctx_t *)ctx;
    const struct hu_config *cfg = pctx ? pctx->cfg : NULL;
    return derive(cfg, HU_BUILT_WITH_RL_FULL != 0, s_reason_buf, sizeof(s_reason_buf),
                  s_detail_json_buf, sizeof(s_detail_json_buf));
}

/* Test seam — uses its own static buffers so a test that asserts on the
 * PRODUCTION runner's result in the same suite doesn't see its buffers
 * overwritten. */
hu_doctor_check_result_t
hu_doctor_check_reaction_collection_wired_run_for_test(const struct hu_config *cfg,
                                                       bool built_with_rl_full) {
    static char t_reason[512];
    static char t_detail[512];
    return derive(cfg, built_with_rl_full, t_reason, sizeof(t_reason), t_detail, sizeof(t_detail));
}

hu_doctor_check_t hu_doctor_check_reaction_collection_wired = {
    .name = "reaction_collection_wired",
    .description =
        "Verifies reaction_collection config + binary-flag agreement (silent-fail guard)",
    .run = check_run,
    .fix = NULL, /* No autofix — operator either edits config OR rebuilds */
    .user_data = NULL,
};
