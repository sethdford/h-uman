/* US-7.10 — RL trainer dispatcher.
 *
 * Thin wrapper around the `hu_rl_trainer_t` vtable. Owns:
 *   - `hu_rl_trainer_deinit` (idempotent helper)
 *   - `hu_rl_trainer_type_name` (used by CLI for error messages)
 *
 * The loss head factories (`hu_rl_trainer_simpo_create`, future
 * `hu_rl_trainer_orpo_create`, `hu_rl_trainer_grpo2_create`) live in
 * their own translation units to keep this file tiny.
 */
#include "human/ml/rl_trainer.h"

#include <stddef.h>

void hu_rl_trainer_deinit(hu_rl_trainer_t *trainer) {
    if (!trainer)
        return;
    if (trainer->vtable && trainer->vtable->deinit)
        trainer->vtable->deinit(trainer->ctx);
    trainer->vtable = NULL;
    trainer->ctx = NULL;
}

const char *hu_rl_trainer_type_name(hu_rl_trainer_type_t type) {
    switch (type) {
    case HU_RL_TRAINER_DPO:
        return "dpo";
    case HU_RL_TRAINER_SIMPO:
        return "simpo";
    case HU_RL_TRAINER_ORPO:
        return "orpo";
    case HU_RL_TRAINER_GRPO2:
        return "grpo2";
    }
    return "unknown";
}
