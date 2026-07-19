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

#ifdef HU_ENABLE_SQLITE
#include "human/agent/reaction_handler.h" /* hu_reaction_handler_lookup_db_probe */
#endif

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

/* Pure verdict logic — takes all three facts explicitly so the public
 * vtable runner and the test seam share one code path. Buffers are caller-
 * provided so the test seam can hand in its own (avoiding race on the
 * static buffers when tests run interleaved with the production check).
 *
 * store_probe: 1 = lookup store opened+migrated, 0 = open FAILED (the
 * bricked-db state PR #321 fixed), -1 = not probed (no SQLite store in
 * this build, or an earlier fact already decided the verdict). */
static hu_doctor_check_result_t derive(const struct hu_config *cfg, bool built_with_rl_full,
                                       int store_probe, char *reason_buf, size_t reason_cap,
                                       char *detail_buf, size_t detail_cap) {
    if (!cfg) {
        return (hu_doctor_check_result_t){
            HU_DOCTOR_NA, "no config provided to doctor — reaction_collection_wired check skipped",
            NULL};
    }

    bool cfg_enabled = cfg->reaction_collection.enabled;
    const char *store_str = store_probe == 1 ? "ok" : (store_probe == 0 ? "fail" : "unprobed");

    snprintf(detail_buf, detail_cap,
             "{\"reaction_collection_enabled\":%s,\"built_with_rl_full\":%s,"
             "\"lookup_store\":\"%s\"}",
             cfg_enabled ? "true" : "false", built_with_rl_full ? "true" : "false", store_str);

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

    if (store_probe == 0) {
        /* Config + compile flag agree, but the store itself cannot open —
         * the exact 2026-05-31 → 2026-07-19 silent failure: registration
         * and tapback lookup both no-op while everything LOOKS wired. */
        snprintf(reason_buf, reason_cap,
                 "reaction_collection is enabled and compiled in, but the lookup store "
                 "~/.human/reaction_lookup.db cannot be opened/migrated — outbound "
                 "registration and tapback lookups are silently no-oping, so no "
                 "imessage_tapback DPO pairs will be recorded. Inspect it with: "
                 "sqlite3 ~/.human/reaction_lookup.db 'PRAGMA integrity_check' — "
                 "if corrupt, move the file aside and restart the daemon (the store "
                 "is recreated on next open; only 60-day lookup history is lost).");
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, reason_buf, detail_buf};
    }

    snprintf(reason_buf, reason_cap,
             "reaction_collection.enabled=true AND binary built with HU_ENABLE_RL_FULL "
             "AND lookup store %s — DPO recorder is wired and will fire on outbound + "
             "tapback pairs.",
             store_probe == 1 ? "opens cleanly" : "not probed (no SQLite in this build)");
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, reason_buf, detail_buf};
}

static hu_doctor_check_result_t check_run(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    const hu_doctor_check_reaction_collection_wired_ctx_t *pctx =
        (const hu_doctor_check_reaction_collection_wired_ctx_t *)ctx;
    const struct hu_config *cfg = pctx ? pctx->cfg : NULL;

    /* Probe the real store ONLY when the two static facts would otherwise
     * PASS — probing earlier would create ~/.human/reaction_lookup.db as a
     * side effect even when the operator opted the subsystem out. */
    int store_probe = -1;
#ifdef HU_ENABLE_SQLITE
    if (cfg && cfg->reaction_collection.enabled && HU_BUILT_WITH_RL_FULL)
        store_probe = hu_reaction_handler_lookup_db_probe();
#endif

    return derive(cfg, HU_BUILT_WITH_RL_FULL != 0, store_probe, s_reason_buf, sizeof(s_reason_buf),
                  s_detail_json_buf, sizeof(s_detail_json_buf));
}

/* Test seam — uses its own static buffers so a test that asserts on the
 * PRODUCTION runner's result in the same suite doesn't see its buffers
 * overwritten. */
hu_doctor_check_result_t
hu_doctor_check_reaction_collection_wired_run_for_test(const struct hu_config *cfg,
                                                       bool built_with_rl_full, int store_probe) {
    static char t_reason[512];
    static char t_detail[512];
    return derive(cfg, built_with_rl_full, store_probe, t_reason, sizeof(t_reason), t_detail,
                  sizeof(t_detail));
}

hu_doctor_check_t hu_doctor_check_reaction_collection_wired = {
    .name = "reaction_collection_wired",
    .description =
        "Verifies reaction_collection config + binary-flag agreement (silent-fail guard)",
    .run = check_run,
    .fix = NULL, /* No autofix — operator either edits config OR rebuilds */
    .user_data = NULL,
};
