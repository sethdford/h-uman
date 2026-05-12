/* tests/test_cli_grpo.c — Phase 4 Tasks 9 + 10 (RL SOTA)
 *
 * Pins the surface contract for `human ml grpo-train` (hu_ml_cli_grpo_train).
 *
 * Test inventory:
 *   1. test_cli_grpo_train_rejects_no_args
 *      — argc==0 must surface the R9 reward-hacking error, not
 *        silently default to "synthetic". (No CLI args means the user
 *        never named a reward source.)
 *   2. test_cli_grpo_train_rejects_n_rollouts_below_2
 *      — R12 / critic M1: GRPO with N=1 has zero group-baseline std,
 *        so there is no gradient signal. Reject at parse time.
 *   3. test_cli_grpo_train_rejects_no_reward_fn_no_reward_model
 *      — Umbrella §10 R9 reward-hacking guard: no implicit default.
 *   4. test_cli_grpo_train_rejects_reward_fn_rm_without_model_path
 *      — Explicit error pairing (-rm flag without checkpoint dir).
 *   5. test_cli_grpo_train_rejects_backend_mlx_without_backbone
 *      — Phase 3 cli_rm precedent for the MLX subprocess argument shape.
 *   6. test_cli_grpo_train_synthetic_reward_smoke_huml_completes_and_writes_adapter
 *      — Umbrella §5 ship contract end-to-end for the HUML backend:
 *        a valid GRPO trainer with synthetic reward runs 5 iters on
 *        the synthetic fixture and writes a non-empty adapter file.
 *   7. test_cli_grpo_train_kl_beta_zero_disables_kl_penalty
 *      — Critic MED-1 escape valve: --kl-beta 0 disables the KL term
 *        end-to-end (the trainer-side test_grpo_huml.c suite pins the
 *        ref_forward_count==0 contract; this CLI smoke confirms the
 *        flag flows through correctly).
 *   8. test_cli_grpo_rm_backed_reward_loads_phase3_checkpoint   (Task 10)
 *      — Umbrella §10 R9 reward-hacking guard at the integration
 *        level: --reward-fn rm --reward-model <fixture> loads a Phase
 *        3 RM checkpoint via hu_reward_model_load, wraps it in
 *        hu_reward_source_create_rm, swaps it onto the trainer via
 *        hu_grpo_set_reward_source, and runs to HU_OK.
 *   9. test_cli_grpo_rm_path_errors_clearly_on_missing_checkpoint (Task 10)
 *      — The --reward-fn rm path produces a clean error code (not a
 *        segfault, not a silent fallback to synthetic) when the
 *        checkpoint directory does not exist.
 *
 * Out of scope (deferred):
 *   - `--reward-fn judge` (Phase 5 territory).
 *   - MLX-backend RM path (mlx subprocess scores in Python; setter
 *     surfaces HU_ERR_NOT_SUPPORTED on that path).
 */

#include "test_framework.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/cli_grpo.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *kGrpoFixture = "tests/fixtures/synthetic_grpo_prompts.jsonl";

/* Best-effort cleanup helper — silent on ENOENT so tests stay idempotent. */
static void unlink_quiet(const char *path) {
    if (path) (void)unlink(path);
}

static void test_cli_grpo_train_rejects_no_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {NULL};
    hu_error_t err = hu_ml_cli_grpo_train(&alloc, 0, argv);
    /* R9: no --reward-fn, no --reward-model → invalid. */
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_cli_grpo_train_rejects_n_rollouts_below_2(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "--pairs", "tests/fixtures/synthetic_grpo_prompts.jsonl",
        "--reward-fn", "synthetic",
        "--rollouts", "1",
    };
    hu_error_t err = hu_ml_cli_grpo_train(&alloc, 6, argv);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_cli_grpo_train_rejects_no_reward_fn_no_reward_model(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "--pairs", "tests/fixtures/synthetic_grpo_prompts.jsonl",
        "--rollouts", "4",
    };
    hu_error_t err = hu_ml_cli_grpo_train(&alloc, 4, argv);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_cli_grpo_train_rejects_reward_fn_rm_without_model_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "--pairs", "tests/fixtures/synthetic_grpo_prompts.jsonl",
        "--reward-fn", "rm",
        "--rollouts", "4",
    };
    hu_error_t err = hu_ml_cli_grpo_train(&alloc, 6, argv);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_cli_grpo_train_rejects_backend_mlx_without_backbone(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "--pairs", "tests/fixtures/synthetic_grpo_prompts.jsonl",
        "--reward-fn", "synthetic",
        "--rollouts", "4",
        "--backend", "mlx",
    };
    hu_error_t err = hu_ml_cli_grpo_train(&alloc, 8, argv);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_cli_grpo_train_synthetic_reward_smoke_huml_completes_and_writes_adapter(void) {
    /* Umbrella §5 row 4 ship contract — HUML side:
     *   ./build/human ml grpo-train --rollouts 4 --pairs <jsonl>
     *     --reward-fn synthetic --iters 5 --backend huml --adapter-out <path>
     * MUST produce a non-empty adapter file at <path>.
     *
     * --adapter-out is a FILE path for the HUML backend (lm_head bytes
     * written directly via fwrite — see src/ml/grpo.c::grpo_huml_save).
     * The MLX backend treats it as a directory; that's exercised by
     * tests/test_grpo_mlx.c::test_grpo_mlx_subprocess_produces_safetensors
     * under HU_HAVE_MLX_LM_GRPO=1, NOT here. */
    const char *adapter_path = "/tmp/hu_test_cli_grpo_synthetic_adapter.bin";
    unlink_quiet(adapter_path);

    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "--pairs", kGrpoFixture,
        "--reward-fn", "synthetic",
        "--rollouts", "4",
        "--iters", "5",
        "--backend", "huml",
        "--adapter-out", adapter_path,
    };
    hu_error_t err = hu_ml_cli_grpo_train(&alloc, 12, argv);
    HU_ASSERT_EQ(err, HU_OK);

    FILE *f = fopen(adapter_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        HU_ASSERT_GT(sz, 0);
    }
    unlink_quiet(adapter_path);
}

static void test_cli_grpo_train_kl_beta_zero_disables_kl_penalty(void) {
    /* Critic MED-1 escape valve: --kl-beta 0 disables the KL penalty
     * end-to-end. The trainer-side test
     * (tests/test_grpo_huml.c::test_grpo_huml_step_with_kl_beta_zero_skips
     * _reference_forward) is the actual contract pin via
     * hu_grpo_huml_ref_forward_count_for_test; here we just smoke that
     * the flag flows through the CLI and the trainer still completes. */
    const char *adapter_path = "/tmp/hu_test_cli_grpo_klbeta_zero_adapter.bin";
    unlink_quiet(adapter_path);

    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "--pairs", kGrpoFixture,
        "--reward-fn", "synthetic",
        "--rollouts", "4",
        "--iters", "5",
        "--backend", "huml",
        "--kl-beta", "0",
        "--adapter-out", adapter_path,
    };
    hu_error_t err = hu_ml_cli_grpo_train(&alloc, 14, argv);
    HU_ASSERT_EQ(err, HU_OK);

    FILE *f = fopen(adapter_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    if (f) fclose(f);
    unlink_quiet(adapter_path);
}

/* ─── Phase 4 Task 10 additions ────────────────────────────────────── */

static void test_cli_grpo_rm_backed_reward_loads_phase3_checkpoint(void) {
    /* The --reward-fn rm path must load the Phase 3 RM checkpoint
     * fixture (tests/fixtures/rm_synthetic_checkpoint/, built by
     * scripts/build-rm-fixture.sh), wrap it in a reward_source, swap
     * it onto the GRPO trainer, and run a small step loop to HU_OK.
     *
     * This is the umbrella §10 R9 reward-hacking guard at the
     * integration level: the user explicitly named the RM as the
     * reward signal, and the CLI must actually wire it through —
     * silently falling back to synthetic would defeat the guard.
     *
     * --adapter-out points at /tmp/... so the test doesn't write a
     * stray ./grpo-adapters into the workspace root (matches the
     * hygiene pattern of tests 6 and 7 above). */
    const char *adapter_path = "/tmp/hu_test_cli_grpo_rm_adapter.bin";
    unlink_quiet(adapter_path);

    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "--pairs",        "tests/fixtures/synthetic_grpo_prompts.jsonl",
        "--reward-fn",    "rm",
        "--reward-model", "tests/fixtures/rm_synthetic_checkpoint",
        "--rollouts",     "4",
        "--iters",        "3",
        "--backend",      "huml",
        "--adapter-out",  adapter_path,
    };
    hu_error_t err = hu_ml_cli_grpo_train(&alloc, 14, argv);
    HU_ASSERT_EQ(err, HU_OK);
    unlink_quiet(adapter_path);
}

static void test_cli_grpo_rm_path_errors_clearly_on_missing_checkpoint(void) {
    /* When --reward-model points at a nonexistent directory the CLI
     * must error cleanly (HU_ERR_IO from missing rm_meta.json, or
     * HU_ERR_PARSE / HU_ERR_INVALID_ARGUMENT on a corrupted file). It
     * must NOT segfault, must NOT silently swallow the error and
     * proceed against the trainer's default synthetic reward source.
     * Any non-HU_OK code satisfies the guard. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "--pairs",        "tests/fixtures/synthetic_grpo_prompts.jsonl",
        "--reward-fn",    "rm",
        "--reward-model", "/nonexistent/path/that/does/not/exist",
        "--rollouts",     "4",
        "--backend",      "huml",
    };
    hu_error_t err = hu_ml_cli_grpo_train(&alloc, 10, argv);
    HU_ASSERT_NEQ(err, HU_OK);
}

void run_cli_grpo_tests(void) {
    HU_TEST_SUITE("cli_grpo");
    HU_RUN_TEST(test_cli_grpo_train_rejects_no_args);
    HU_RUN_TEST(test_cli_grpo_train_rejects_n_rollouts_below_2);
    HU_RUN_TEST(test_cli_grpo_train_rejects_no_reward_fn_no_reward_model);
    HU_RUN_TEST(test_cli_grpo_train_rejects_reward_fn_rm_without_model_path);
    HU_RUN_TEST(test_cli_grpo_train_rejects_backend_mlx_without_backbone);
    HU_RUN_TEST(test_cli_grpo_train_synthetic_reward_smoke_huml_completes_and_writes_adapter);
    HU_RUN_TEST(test_cli_grpo_train_kl_beta_zero_disables_kl_penalty);
    HU_RUN_TEST(test_cli_grpo_rm_backed_reward_loads_phase3_checkpoint);
    HU_RUN_TEST(test_cli_grpo_rm_path_errors_clearly_on_missing_checkpoint);
}
