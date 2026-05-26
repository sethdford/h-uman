/* include/human/doctor/check_reaction_collection_wired.h
 *
 * 2026-05 audit follow-up — doctor check that catches a class of silent
 * failure where the CONFIG enables a subsystem but the BINARY was built
 * without the compile flag that wires the subsystem's writers in.
 *
 * Specific bug this prevents (h-uman 2026-05-26 audit, F#4):
 *   - `~/.human/config.json::reaction_collection.enabled = true`
 *   - Binary built with `HU_ENABLE_RL_FULL=OFF` (the CMake default)
 *   - Result: `src/daemon.c:12745`'s `hu_reaction_handler_register_
 *     assistant_message_for_production` call is compiled out
 *   - Daemon polls reactions, sees them, then silently drops every one
 *     at the lookup step (no recorder ran on the outbound, so the
 *     lookup table is empty)
 *   - `dpo_pairs.source = 'imessage_tapback'` row count stays at 0
 *     for weeks while the operator believes the feature is live
 *
 * The runtime sibling rule `~/.claude/rules/silent-config-gated-
 * subsystems.md` covers the case where a runtime CONFIG bool disables
 * a subsystem; this check extends the same discipline to COMPILE-TIME
 * gates that the operator can't see from config alone.
 *
 * Verdict semantics:
 *   NA   — cfg->reaction_collection.enabled == false
 *          (operator opted out; nothing to check)
 *   FAIL — cfg.enabled == true AND binary built WITHOUT HU_ENABLE_RL_FULL
 *          (silent-failure state — message names the exact fix command)
 *   PASS — cfg.enabled == true AND binary built WITH HU_ENABLE_RL_FULL
 *          (subsystem will produce DPO pairs when reactions arrive)
 *
 * ctx contract: same shape as check_prompt_budget — a borrowed
 * `const struct hu_config *`. NULL cfg → NA "no config".
 */
#ifndef HU_DOCTOR_CHECK_REACTION_COLLECTION_WIRED_H
#define HU_DOCTOR_CHECK_REACTION_COLLECTION_WIRED_H

#include "human/core/error.h"
#include "human/doctor/check.h"

struct hu_config;

typedef struct hu_doctor_check_reaction_collection_wired_ctx {
    const struct hu_config *cfg;
} hu_doctor_check_reaction_collection_wired_ctx_t;

/* Public vtable — registered by registry.c::register_defaults. */
extern hu_doctor_check_t hu_doctor_check_reaction_collection_wired;

/* Test seam: invoke the check logic with an EXPLICIT `built_with_rl_full`
 * value so tests can cover both production-shape and silent-fail-shape
 * verdicts in a single binary (the production runner reads the value
 * from a compile-time #ifdef of THIS .c file, which can't be changed
 * per-test). Production code MUST call the vtable above, not this. */
hu_doctor_check_result_t
hu_doctor_check_reaction_collection_wired_run_for_test(const struct hu_config *cfg,
                                                       bool built_with_rl_full);

#endif /* HU_DOCTOR_CHECK_REACTION_COLLECTION_WIRED_H */
