/* src/ml/dpo_real_huml.c — STUB until Phase 2 Task 4
 *
 * Task 4 replaces this with the real in-process HUML DPO trainer
 * (frozen π_ref + finite-difference grad check + sign-of-gradient
 * E2E test on synthetic preference pairs). Until then, the factory
 * returns HU_ERR_NOT_SUPPORTED so callers (and the rl_trainer test
 * suite) can distinguish "not implemented yet" from "broken".
 */
#include "human/ml/dpo_real.h"

hu_error_t hu_dpo_real_huml_create(hu_allocator_t *a, const hu_rl_trainer_config_t *c, hu_rl_trainer_t *o) {
    (void)a; (void)c; (void)o; return HU_ERR_NOT_SUPPORTED;
}
