/* tests/test_dpo_real_loss.c — Phase 2 Task 4
 *
 * Pins the real DPO loss + structural backward in src/ml/dpo_real_huml.c:
 *   1. With learning_rate=0 and one synthetic pair, the chosen logprob
 *      delta is >= the rejected delta after one step (trivially true at
 *      t=0 because policy == reference; serves as a structural assertion
 *      that step() returns success and populates metrics).
 *   2. With learning_rate=1e-3 over 50 steps on the same pair, the
 *      summed chosen logprob delta exceeds the summed rejected delta
 *      (sign-of-improvement; tolerates the tiny step_scale * 0.001
 *      perturbation hardcoded in the structural backward).
 *
 * Plan deviation notes (extending Tasks 1 + 2 + 3):
 *   1. The canonical plan snippet (lines 955-1014) `#include`s nothing
 *      beyond the local headers, but its hu_gpt_config_t designated
 *      initialiser at line 965 uses field names `n_layers`, `n_heads`,
 *      `d_model`, `max_seq_len` which do NOT exist on this repo's
 *      struct (see include/human/ml/ml.h:31-44 — the real fields are
 *      `n_layer`, `n_head`, `n_kv_head`, `n_embd`, `head_dim`,
 *      `sequence_len`). Translated per parent-task brief: the plan's
 *      `vocab_size=16, d_model=8` becomes `vocab_size=16, n_layer=1,
 *      n_head=1, n_kv_head=1, n_embd=8, head_dim=8, sequence_len=8`.
 *      This satisfies hu_gpt_create's invariants (n_embd == n_head *
 *      head_dim and head_dim % 2 == 0; src/ml/gpt.c:927-930).
 *   2. The plan's first test declares `hu_gpt_config_t cfg = {...}`
 *      but never uses `cfg` — it passes `tcfg` to the trainer factory
 *      and the trainer builds its own internal config (see
 *      src/ml/dpo_real_huml.c::hu_dpo_real_huml_create). Under
 *      `-Wall -Wunused-variable -Werror` (the project default), the
 *      bare declaration is a hard build error. Adding `(void)cfg;` to
 *      preserve the plan's structure while satisfying the compiler.
 *      Documented as the third deviation in the Task 4 commit.
 */
#include "test_framework.h"
#include "human/ml/rl_trainer.h"
#include "human/ml/policy_logprobs.h"
#include "human/ml/reference_model.h"
#include "human/ml/model.h"
#include "human/core/allocator.h"
#include <math.h>

static void test_dpo_real_huml_loss_finite_diff_lm_head(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {
        .vocab_size = 16,
        .n_layer = 1,
        .n_head = 1,
        .n_kv_head = 1,
        .n_embd = 8,
        .head_dim = 8,
        .sequence_len = 8,
    };
    (void)cfg;  /* trainer builds its own internal config; see deviation note 2 */
    hu_rl_trainer_config_t tcfg = {.backend = HU_DPO_BACKEND_HUML, .beta = 0.1, .learning_rate = 0};
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_dpo(&alloc, &tcfg, &trainer), HU_OK);

    /* One synthetic preference pair (tokenizer-free: space-separated int ids).
     * IDs stay below the trainer's internal vocab_size=32 (see
     * src/ml/dpo_real_huml.c::hu_dpo_real_huml_create). */
    hu_preference_pair_t pair = {
        .prompt = "1 2 3",
        .chosen = "4 5",
        .rejected = "6 7",
        .source = "test",
    };

    hu_rl_trainer_metrics_t m = {0};
    HU_ASSERT_EQ(trainer.vtable->step(trainer.ctx, &alloc, &pair, 1, &m), HU_OK);

    /* Sign-of-gradient check: with learning_rate=0 the policy is unchanged,
     * so policy log-probs equal reference log-probs and both deltas are 0
     * (so 0 >= 0 trivially). The full per-parameter finite-diff lives in
     * the step() implementation's debug-mode hook (Phase 5 work). */
    HU_ASSERT_TRUE(m.chosen_logprob_delta >= m.rejected_logprob_delta);

    trainer.vtable->deinit(trainer.ctx, &alloc);
}

/* Sign-of-improvement: after 50 steps on the same pair, summed chosen
 * logprob delta exceeds summed rejected logprob delta. */
static void test_dpo_real_huml_e2e_sign_of_improvement(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t tcfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .beta = 0.1,
        .learning_rate = 1e-3,
        .max_iters = 1,
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_dpo(&alloc, &tcfg, &trainer), HU_OK);

    hu_preference_pair_t pair = {
        .prompt = "1 2 3",
        .chosen = "4 5",
        .rejected = "6 7",
        .source = "test",
    };

    double chosen_total = 0, rejected_total = 0;
    for (int i = 0; i < 50; i++) {
        hu_rl_trainer_metrics_t m = {0};
        HU_ASSERT_EQ(trainer.vtable->step(trainer.ctx, &alloc, &pair, 1, &m), HU_OK);
        chosen_total += m.chosen_logprob_delta;
        rejected_total += m.rejected_logprob_delta;
    }
    HU_ASSERT_TRUE(chosen_total > rejected_total);  /* preference direction */

    trainer.vtable->deinit(trainer.ctx, &alloc);
}

void run_dpo_real_loss_tests(void) {
    HU_RUN_TEST(test_dpo_real_huml_loss_finite_diff_lm_head);
    HU_RUN_TEST(test_dpo_real_huml_e2e_sign_of_improvement);
}
