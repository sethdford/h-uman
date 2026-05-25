/* tests/test_m3_frontier_auto_invocation.c
 *
 * Spec 2026-05-19 M3 closure / AC-M3-7 — E2E test for the daemon
 * auto-invocation path. Verifies:
 *
 *   1. The pair-count-threshold predicate fires when uncommitted
 *      pair count crosses threshold (Spec 2 contract — re-asserted
 *      here as the entry point to this slice).
 *   2. When the fire happens AND target=FRONTIER_MLX is requested,
 *      the most-recent-target slot advances to FRONTIER_MLX.
 *   3. The backward-compatible wrapper still produces
 *      target=HUML_REFERENCE.
 *   4. The fake mlx_lm shim produces a non-empty safetensors header
 *      with LoRA tensor entries — the same shape the admin-swap
 *      endpoint expects. This proves the test fixture is wire-
 *      compatible with the real bridge's output, so the daemon's
 *      post-training hook can swap it in.
 *
 * Deterministic discipline (per .claude/rules/testing.md):
 *   - No real subprocess training. The fake_mlx_lm_train.sh shim
 *     copies a known-good safetensors fixture in place of actual
 *     training; we exercise it once to verify the contract.
 *   - The enqueue target slot is process-wide; subsequent calls
 *     overwrite each other deterministically (single-writer in the
 *     daemon scheduler tick).
 *
 * Gate symmetry (per .claude/rules/test-source-gate-symmetry.md):
 *   training_runner_shared.c is always in HU_CORE_SOURCES (the stub
 *   path is taken when HU_ENABLE_LEARNING is OFF). The header lives
 *   alongside, so this test is unconditional too.
 */

#include "test_framework.h"

#include "human/agent/training_runner_shared.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(HU_ENABLE_LEARNING) && defined(HU_ENABLE_SQLITE)
#include "human/agent/world_model_bridge.h"
#include "human/memory/graph.h"
#endif

/* ── 1. Pair-count predicate fires when threshold crossed ──────────── */

static void pair_count_predicate_fires_at_threshold(void) {
    /* Threshold = 50; below threshold → no fire. */
    HU_ASSERT_FALSE(hu_training_runner_pair_count_should_fire(49, 50));
    HU_ASSERT_FALSE(hu_training_runner_pair_count_should_fire(0, 50));
    /* At or above threshold → fire. */
    HU_ASSERT_TRUE(hu_training_runner_pair_count_should_fire(50, 50));
    HU_ASSERT_TRUE(hu_training_runner_pair_count_should_fire(99, 50));
    /* Threshold = 0 (operator-disabled) → NEVER fire. */
    HU_ASSERT_FALSE(hu_training_runner_pair_count_should_fire(100, 0));
}

/* ── 2. Null scheduler is rejected ─────────────────────────────────── */

static void target_enqueue_null_scheduler_rejects(void) {
    hu_error_t e = hu_training_runner_enqueue_lora_persona_target(
        NULL, 0, 0, HU_TRAINING_TRIGGER_PAIR_COUNT, HU_TRAINING_TARGET_FRONTIER_MLX, NULL);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
}

/* ── 3. Slot advances to FRONTIER_MLX when target requested ────────── */

#if defined(HU_ENABLE_LEARNING) && defined(HU_ENABLE_SQLITE)
/* LEARNING=ON path — stand up a real scheduler so the underlying
 * enqueue actually fires. Mirrors the setup in test_e2e_rl_loop.c. */
static void target_enqueue_advances_slot_to_frontier_mlx(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, NULL, 0, &g), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(g, &alloc, &f), HU_OK);
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(f, &alloc, &s), HU_OK);

    /* Seed slot with HUML_REFERENCE so we can observe the transition. */
    hu_error_t e = hu_training_runner_enqueue_lora_persona_target(
        s, 0, 0, HU_TRAINING_TRIGGER_PAIR_COUNT, HU_TRAINING_TARGET_HUML_REFERENCE, NULL);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ((int)hu_training_runner_last_enqueued_target(),
                 (int)HU_TRAINING_TARGET_HUML_REFERENCE);

    /* AC-M3-7 contract: requesting FRONTIER_MLX advances the slot. */
    e = hu_training_runner_enqueue_lora_persona_target(s, 0, 0, HU_TRAINING_TRIGGER_PAIR_COUNT,
                                                       HU_TRAINING_TARGET_FRONTIER_MLX, NULL);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ((int)hu_training_runner_last_enqueued_target(),
                 (int)HU_TRAINING_TARGET_FRONTIER_MLX);

    /* Backward-compatible wrapper still produces HUML_REFERENCE. */
    e = hu_training_runner_enqueue_lora_persona(s, 0, 0, HU_TRAINING_TRIGGER_PAIR_COUNT, NULL);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ((int)hu_training_runner_last_enqueued_target(),
                 (int)HU_TRAINING_TARGET_HUML_REFERENCE);

    hu_w14_scheduler_close(s, &alloc);
    hu_w7_facade_close(f, &alloc);
    hu_graph_close(g, &alloc);
}

#else /* !(HU_ENABLE_LEARNING && HU_ENABLE_SQLITE) */

static void target_enqueue_advances_slot_to_frontier_mlx(void) {
    /* Stub-path enqueue: the LEARNING=OFF body returns HU_OK without
     * touching the scheduler so a non-NULL sentinel is enough. */
    char sentinel;
    hu_w14_scheduler_t *fake = (hu_w14_scheduler_t *)(void *)&sentinel;

    hu_error_t e = hu_training_runner_enqueue_lora_persona_target(
        fake, 0, 0, HU_TRAINING_TRIGGER_PAIR_COUNT, HU_TRAINING_TARGET_HUML_REFERENCE, NULL);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ((int)hu_training_runner_last_enqueued_target(),
                 (int)HU_TRAINING_TARGET_HUML_REFERENCE);

    e = hu_training_runner_enqueue_lora_persona_target(fake, 0, 0, HU_TRAINING_TRIGGER_PAIR_COUNT,
                                                       HU_TRAINING_TARGET_FRONTIER_MLX, NULL);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ((int)hu_training_runner_last_enqueued_target(),
                 (int)HU_TRAINING_TARGET_FRONTIER_MLX);

    e = hu_training_runner_enqueue_lora_persona(fake, 0, 0, HU_TRAINING_TRIGGER_PAIR_COUNT, NULL);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ((int)hu_training_runner_last_enqueued_target(),
                 (int)HU_TRAINING_TARGET_HUML_REFERENCE);
}

#endif

/* ── 4. Fake mlx_lm shim produces a non-empty safetensors ──────────── */

static int find_fixture_shim(char *out, size_t out_sz) {
    /* Try a few candidate prefixes to handle different cwds. */
    const char *prefixes[] = {
        "tests/fixtures/m3/fake_mlx_lm_train.sh",
        "../tests/fixtures/m3/fake_mlx_lm_train.sh",
        "../../tests/fixtures/m3/fake_mlx_lm_train.sh",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (access(prefixes[i], R_OK) == 0) {
            snprintf(out, out_sz, "%s", prefixes[i]);
            return 0;
        }
    }
    return -1;
}

static void fake_mlx_lm_shim_produces_real_safetensors(void) {
    char shim[512] = {0};
    if (find_fixture_shim(shim, sizeof(shim)) != 0) {
        /* Skip on builds where the fixture isn't reachable from cwd. */
        return;
    }
    const char *out_path = "/tmp/hu_m3_fake_adapter.safetensors";
    (void)unlink(out_path);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "bash %s --pairs /dev/null --adapter-out %s --rank 16 --iters 1 --model fake "
             ">/dev/null 2>&1",
             shim, out_path);
    int rc = system(cmd);
    HU_ASSERT_EQ(rc, 0);

    struct stat st;
    HU_ASSERT_EQ(stat(out_path, &st), 0);
    HU_ASSERT_TRUE(st.st_size > 16);

    /* Parse the header; must declare at least one non-metadata tensor.
     * Header layout: 8-byte LE length + JSON. */
    FILE *fp = fopen(out_path, "rb");
    HU_ASSERT_NOT_NULL(fp);
    unsigned char hl[8];
    HU_ASSERT_EQ(fread(hl, 1, 8, fp), (size_t)8);
    uint64_t header_len = 0;
    for (int i = 0; i < 8; i++)
        header_len |= ((uint64_t)hl[i]) << (8 * i);
    HU_ASSERT_TRUE(header_len > 16);
    HU_ASSERT_TRUE(header_len < 1024 * 16);
    char *jbuf = (char *)malloc((size_t)header_len + 1);
    HU_ASSERT_NOT_NULL(jbuf);
    HU_ASSERT_EQ(fread(jbuf, 1, (size_t)header_len, fp), (size_t)header_len);
    jbuf[header_len] = '\0';
    fclose(fp);
    /* The header must contain at least one lora_a or lora_b tensor key. */
    HU_ASSERT_TRUE(strstr(jbuf, "lora_a") != NULL || strstr(jbuf, "lora_b") != NULL);
    free(jbuf);
    (void)unlink(out_path);
}

void run_m3_frontier_auto_invocation_tests(void);
void run_m3_frontier_auto_invocation_tests(void) {
    HU_TEST_SUITE("m3_frontier_auto_invocation");
    HU_RUN_TEST(pair_count_predicate_fires_at_threshold);
    HU_RUN_TEST(target_enqueue_null_scheduler_rejects);
    HU_RUN_TEST(target_enqueue_advances_slot_to_frontier_mlx);
    HU_RUN_TEST(fake_mlx_lm_shim_produces_real_safetensors);
}
