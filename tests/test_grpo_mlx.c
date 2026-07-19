/* tests/test_grpo_mlx.c — Phase 4 Task 8 (RL SOTA)
 *
 * Pins the GRPO MLX subprocess trainer in src/ml/grpo_mlx.c.
 *
 * Test count by build flag:
 *   - HU_HAVE_MLX_LM_GRPO undefined (CI default): 4 tests, all PASS.
 *     The subprocess test prints [skip] and returns without asserting
 *     anything that could fail.
 *   - HU_HAVE_MLX_LM_GRPO=1 (local opt-in): 4 tests; the subprocess
 *     test actually invokes the python wrapper (which, under
 *     HU_IS_TEST + HU_HAVE_MLX_LM_GRPO=1, runs the REAL mlx-lm-lora
 *     GRPO trainer — the C-side HU_E2E_TEST_MODE setenv is suppressed
 *     under HU_HAVE_MLX_LM_GRPO).
 *
 * Round-3 critic fold-in pinned:
 *   M7   create-time probe short-circuits to 0 under HU_IS_TEST so
 *        the M7 contract (no python3 probe subprocesses from tests)
 *        is preserved.  Tested by
 *        test_grpo_mlx_factory_unavailable_when_python_probe_fails.
 *   H2   JSONL write uses O_EXCL + 0600 — symlink-attack resistant,
 *        owner-RW only.  Tested unconditionally by
 *        test_grpo_mlx_jsonl_write_uses_secure_perms.
 *   L1   subprocess test gated by #if defined(HU_HAVE_MLX_LM_GRPO) &&
 *        HU_HAVE_MLX_LM_GRPO == 1 — NOT HU_SKIP_IF (round-3 critic L1
 *        — gating is compile-time, not runtime, so dummy binaries
 *        don't ship the heavy path).
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/grpo.h"
#include "human/ml/rl_trainer.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Forward declarations of the test-only seams exposed by
 * src/ml/grpo_mlx.c. Picked up via the human_tests
 * target_include_directories(${HU_ROOT}/src/ml) entry in
 * CMakeLists.txt — same mechanism as Phase 3 RM and Phase 4 GRPO-loss
 * tests. */
hu_error_t hu_grpo_mlx_write_jsonl_for_test(const char *out_path, const hu_preference_pair_t *pairs,
                                            size_t n_pairs);
hu_error_t hu_grpo_mlx_create_for_test(hu_allocator_t *alloc, const hu_rl_trainer_config_t *config,
                                       hu_rl_trainer_t *out);

/* --- 1. Factory: probe short-circuit (M7) --------------------------- */
static void test_grpo_mlx_factory_unavailable_when_python_probe_fails(void) {
    /* Round-3 critic M7: mlx_lm_lora_grpo_available() short-circuits
     * to 0 under HU_IS_TEST so the test contract is deterministic
     * regardless of whether mlx-lm-lora is installed on the build
     * machine.  Without this guard the test would PASS on CI (no
     * package installed) but FAIL on dev laptops (package installed,
     * probe returns 1, create returns HU_OK). */
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_MLX,
        .max_iters = 5,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.04,
        .model_id = "mlx-community/gemma-3-4b-it-bf16",
        .adapter_out_dir = "/tmp/hu_grpo_mlx_unavailable_test",
    };
    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_grpo_mlx_create(&alloc, &cfg, &trainer);
#if defined(__APPLE__)
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#else
    /* Non-Apple: __APPLE__ guard also returns HU_ERR_NOT_SUPPORTED
     * before the probe runs.  Same expected result, different reason. */
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
    /* Trainer struct must remain {0} when create fails. */
    HU_ASSERT_NULL(trainer.ctx);
    HU_ASSERT_NULL((void *)trainer.vtable);
}

/* --- 2. JSONL secure perms (H2) -------------------------------------- */
static void test_grpo_mlx_jsonl_write_uses_secure_perms(void) {
    /* Round-3 critic H2: O_EXCL + 0600 mode on the JSONL write defeats
     * symlink-attack on the user-controlled out_path AND limits
     * exposure to other local users.  Unconditional test — the
     * hardening must be in effect regardless of HU_HAVE_MLX_LM_GRPO. */
    char path[128];
    snprintf(path, sizeof(path), "/tmp/hu_grpo_mlx_perms_test_%ld.jsonl", (long)getpid());
    /* Remove any stale file from a prior aborted test run. */
    unlink(path);

    hu_preference_pair_t pair = {
        .prompt = "secure perms test",
        .prompt_len = 17,
    };
    hu_error_t err = hu_grpo_mlx_write_jsonl_for_test(path, &pair, 1);
    HU_ASSERT_EQ(err, HU_OK);

    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);
    HU_ASSERT_TRUE(st.st_size > 0);

    /* The H2 contract: mode bits should be exactly 0600 (owner read +
     * write, no group/other access). umask may have masked things off
     * but never on, so checking equality (after masking the file-type
     * bits) is correct. */
    mode_t perms = st.st_mode & 0777;
    HU_ASSERT_EQ(perms, 0600);

    /* Verify only-prompt schema — chosen/rejected MUST be skipped. */
    FILE *f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[256];
    if (fgets(buf, sizeof(buf), f)) {
        HU_ASSERT_STR_CONTAINS(buf, "\"prompt\"");
        HU_ASSERT_STR_CONTAINS(buf, "secure perms test");
        HU_ASSERT_STR_NOT_CONTAINS(buf, "chosen");
        HU_ASSERT_STR_NOT_CONTAINS(buf, "rejected");
    } else {
        HU_FAIL("expected at least one JSONL row written");
    }
    fclose(f);

    unlink(path);
}

/* --- 3. Dummy adapter (HU_E2E_TEST_MODE Python shortcut) ------------- */
static void test_grpo_mlx_dummy_adapter_in_test_mode(void) {
#if !defined(__APPLE__)
    fprintf(stderr, "[skip] non-Apple: GRPO MLX path returns HU_ERR_NOT_SUPPORTED by design\n");
    return;
#else
    /* Use the for-test seam (hu_grpo_mlx_create_for_test) to bypass
     * the M7 probe short-circuit so step() actually runs.  Under
     * HU_IS_TEST + !HU_HAVE_MLX_LM_GRPO the C-side setenvs
     * HU_E2E_TEST_MODE=1 before popen, and the Python wrapper writes a
     * 0-byte sentinel adapter_model.safetensors and exits 0 — no
     * real MLX, no Gemma download, no network. */
    hu_allocator_t alloc = hu_system_allocator();
    char out_dir[128];
    snprintf(out_dir, sizeof(out_dir), "/tmp/hu_grpo_mlx_dummy_test_%ld", (long)getpid());
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_MLX,
        .max_iters = 1,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.04,
        .model_id = "test-model",
        .adapter_out_dir = out_dir,
    };
    hu_rl_trainer_t trainer = {0};
    hu_error_t cerr = hu_grpo_mlx_create_for_test(&alloc, &cfg, &trainer);
    HU_ASSERT_EQ(cerr, HU_OK);
    HU_ASSERT_NOT_NULL(trainer.ctx);
    HU_ASSERT_NOT_NULL((void *)trainer.vtable);

    /* Pre-clean any prior sentinel to make the assertion below
     * meaningful — we want to assert step() wrote it, not that a
     * stale file from yesterday is sitting there. */
    char sentinel[768];
    snprintf(sentinel, sizeof(sentinel), "%s/adapter_model.safetensors", out_dir);
    unlink(sentinel);

    hu_preference_pair_t pair = {
        .prompt = "hello",
        .prompt_len = 5,
    };
    hu_rl_trainer_metrics_t m = {0};
    hu_error_t serr = trainer.vtable->step(trainer.ctx, &alloc, &pair, 1, &m);

#if defined(HU_HAVE_MLX_LM_GRPO) && HU_HAVE_MLX_LM_GRPO == 1
    /* Real subprocess path — depends on python3, mlx-lm-lora, network.
     * Don't assert success deterministically; just ensure the call
     * doesn't crash. The dedicated subprocess test below asserts the
     * real-output contract. */
    fprintf(stderr, "[grpo_mlx] step returned %d under HU_HAVE_MLX_LM_GRPO=1\n", (int)serr);
    (void)serr;
    (void)m;
#else
    /* Dummy-adapter path — Python wrapper writes 0-byte sentinel. */
    HU_ASSERT_EQ(serr, HU_OK);
    HU_ASSERT_TRUE(strlen(m.adapter_path) > 0);
    HU_ASSERT_STR_CONTAINS(m.adapter_path, "adapter_model.safetensors");
    struct stat st;
    HU_ASSERT_EQ(stat(m.adapter_path, &st), 0);
    /* 0-byte sentinel is the test-mode contract — Python touches the
     * file and exits 0 without writing real bytes. */
    HU_ASSERT_EQ((long long)st.st_size, 0);
#endif

    trainer.vtable->deinit(trainer.ctx, &alloc);
    /* Clean up the sentinel + the temp dir (best-effort; ignore
     * failure if the dir contains other files). */
    unlink(sentinel);
    rmdir(out_dir);
#endif
}

/* --- 4. Real subprocess (gated on HU_HAVE_MLX_LM_GRPO — L1) ---------- */
static void test_grpo_mlx_subprocess_produces_safetensors(void) {
#if !defined(HU_HAVE_MLX_LM_GRPO) || HU_HAVE_MLX_LM_GRPO == 0
    fprintf(
        stderr,
        "[skip] HU_HAVE_MLX_LM_GRPO not defined; GRPO MLX subprocess test deferred to local run\n");
    return;
#elif !defined(__APPLE__)
    fprintf(stderr, "[skip] non-Apple: GRPO MLX subprocess unavailable\n");
    return;
#else
    /* Round-3 critic L1: compile-time gate, NOT HU_SKIP_IF.  This test
     * runs the REAL mlx-lm-lora GRPO subprocess (a few iters on a tiny
     * fixture). Budget ~10s. */
    hu_allocator_t alloc = hu_system_allocator();
    char out_dir[128];
    snprintf(out_dir, sizeof(out_dir), "/tmp/hu_grpo_mlx_subprocess_test_%ld", (long)getpid());
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_MLX,
        .max_iters = 5,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.04,
        .model_id = "mlx-community/gemma-3-4b-it-bf16",
        .adapter_out_dir = out_dir,
    };
    hu_rl_trainer_t trainer = {0};
    /* Use the production factory — under HU_HAVE_MLX_LM_GRPO=1 we
     * intend the M7 short-circuit to NOT fire (it still does — the
     * #if HU_IS_TEST guard is unconditional).  Fall back to the
     * for-test seam if the production path declines. */
    hu_error_t cerr = hu_grpo_mlx_create(&alloc, &cfg, &trainer);
    if (cerr == HU_ERR_NOT_SUPPORTED) {
        cerr = hu_grpo_mlx_create_for_test(&alloc, &cfg, &trainer);
    }
    HU_ASSERT_EQ(cerr, HU_OK);

    hu_preference_pair_t pair = {
        .prompt = "hi",
        .prompt_len = 2,
    };
    hu_rl_trainer_metrics_t m = {0};
    hu_error_t serr = trainer.vtable->step(trainer.ctx, &alloc, &pair, 1, &m);
    HU_ASSERT_EQ(serr, HU_OK);
    HU_ASSERT_TRUE(strlen(m.adapter_path) > 0);

    struct stat st;
    HU_ASSERT_EQ(stat(m.adapter_path, &st), 0);
    /* Real subprocess writes real bytes — non-zero size. */
    HU_ASSERT_TRUE(st.st_size > 0);

    trainer.vtable->deinit(trainer.ctx, &alloc);
#endif
}

void run_grpo_mlx_tests(void) {
    HU_TEST_SUITE("grpo_mlx");
    HU_RUN_TEST(test_grpo_mlx_factory_unavailable_when_python_probe_fails);
    HU_RUN_TEST(test_grpo_mlx_jsonl_write_uses_secure_perms);
    HU_RUN_TEST(test_grpo_mlx_dummy_adapter_in_test_mode);
    HU_RUN_TEST(test_grpo_mlx_subprocess_produces_safetensors);
}
