/* tests/test_lora_subprocess.c
 *
 * Sprint B residuals N1 — mlx_lm.lora subprocess driver.
 * Contracts (10 tests):
 *   build_argv:
 *     1. happy path produces python3 -m mlx_lm.lora --model ... shape
 *     2. NULL cfg / argv_out → 0
 *     3. missing required field (base_model/data/adapter) → 0
 *     4. hyperparams 0 → built-in defaults appear in argv
 *     5. explicit hyperparams override defaults
 *     6. argv_cap too small → 0 (no overflow)
 *   preflight:
 *     7. NULL cfg → false
 *     8. empty base_model → false
 *     9. missing data file → false
 *   end-to-end:
 *    10. hu_lora_subprocess_train with NULL alloc → INVALID_ARGUMENT
 */

#include "human/ml/lora_subprocess.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void make_cfg(hu_lora_subprocess_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->base_model, sizeof(cfg->base_model), "mlx-community/gemma-2-2b-it-4bit");
    snprintf(cfg->data_jsonl_path, sizeof(cfg->data_jsonl_path), "/tmp/lora-data.jsonl");
    snprintf(cfg->adapter_output_dir, sizeof(cfg->adapter_output_dir), "/tmp/lora-adapter");
}

/* ── build_argv ──────────────────────────────────────────────────────── */

static void test_build_argv_happy_path_shape(void) {
    hu_lora_subprocess_config_t cfg;
    make_cfg(&cfg);
    const char *argv[24];
    char buf[2048];
    size_t n = hu_lora_subprocess_build_argv(&cfg, argv, 24, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(argv[0], "python3");
    HU_ASSERT_STR_EQ(argv[1], "-m");
    HU_ASSERT_STR_EQ(argv[2], "mlx_lm.lora");
    /* --model id must appear together */
    bool saw_model_flag = false;
    bool saw_train_flag = false;
    bool saw_adapter_path = false;
    bool saw_num_layers = false;
    bool saw_stale_lora_layers = false;
    bool saw_config_flag = false;
    const char *config_path = NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(argv[i], "--model") == 0)
            saw_model_flag = true;
        if (strcmp(argv[i], "--train") == 0)
            saw_train_flag = true;
        if (strcmp(argv[i], "--adapter-path") == 0)
            saw_adapter_path = true;
        if (strcmp(argv[i], "--num-layers") == 0)
            saw_num_layers = true;
        if (strcmp(argv[i], "--lora-layers") == 0)
            saw_stale_lora_layers = true;
        if (strcmp(argv[i], "-c") == 0 && i + 1 < n) {
            saw_config_flag = true;
            config_path = argv[i + 1];
        }
    }
    HU_ASSERT_TRUE(saw_model_flag);
    HU_ASSERT_TRUE(saw_train_flag);
    HU_ASSERT_TRUE(saw_adapter_path);
    /* 2026-07-05 CLI-drift fix: modern mlx_lm.lora renamed --lora-layers to
     * --num-layers; the stale flag made every nightly train exit 2 with a
     * usage error (3 no-op nights, Jun 12-14). Pin the new flag AND the
     * absence of the old one. */
    HU_ASSERT_TRUE(saw_num_layers);
    HU_ASSERT_TRUE(!saw_stale_lora_layers);
    /* lora-scale-default-or-die: the invocation MUST carry a -c config (which
     * the exec layer writes with lora_parameters scale 2.0) — omitting it
     * inherits mlx_lm's catastrophic scale=20 default. */
    HU_ASSERT_TRUE(saw_config_flag);
    HU_ASSERT_NOT_NULL(config_path);
    HU_ASSERT_TRUE(strstr(config_path, "hu_lora_config.yaml") != NULL);
    /* argv must be NULL-terminated. */
    HU_ASSERT_TRUE(argv[n] == NULL);
}

static void test_build_argv_null_inputs_return_zero(void) {
    hu_lora_subprocess_config_t cfg;
    make_cfg(&cfg);
    const char *argv[24];
    char buf[2048];
    HU_ASSERT_EQ((int)hu_lora_subprocess_build_argv(NULL, argv, 24, buf, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_lora_subprocess_build_argv(&cfg, NULL, 24, buf, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_lora_subprocess_build_argv(&cfg, argv, 24, NULL, sizeof(buf)), 0);
}

static void test_build_argv_missing_required_field_returns_zero(void) {
    hu_lora_subprocess_config_t cfg;
    const char *argv[24];
    char buf[2048];
    /* No base_model. */
    make_cfg(&cfg);
    cfg.base_model[0] = '\0';
    HU_ASSERT_EQ((int)hu_lora_subprocess_build_argv(&cfg, argv, 24, buf, sizeof(buf)), 0);
    /* No data path. */
    make_cfg(&cfg);
    cfg.data_jsonl_path[0] = '\0';
    HU_ASSERT_EQ((int)hu_lora_subprocess_build_argv(&cfg, argv, 24, buf, sizeof(buf)), 0);
    /* No adapter dir. */
    make_cfg(&cfg);
    cfg.adapter_output_dir[0] = '\0';
    HU_ASSERT_EQ((int)hu_lora_subprocess_build_argv(&cfg, argv, 24, buf, sizeof(buf)), 0);
}

static void test_build_argv_zero_hyperparams_use_defaults(void) {
    hu_lora_subprocess_config_t cfg;
    make_cfg(&cfg);
    /* All hyperparams left as 0 → defaults from impl should appear. */
    const char *argv[24];
    char buf[2048];
    size_t n = hu_lora_subprocess_build_argv(&cfg, argv, 24, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    /* The defaults are batch=4, iters=200, lora_layers=8. We verify
     * the digit strings appear immediately after their flags. */
    bool batch_default_seen = false, iters_default_seen = false, layers_default_seen = false;
    for (size_t i = 0; i + 1 < n; i++) {
        if (strcmp(argv[i], "--batch-size") == 0 && strcmp(argv[i + 1], "4") == 0)
            batch_default_seen = true;
        if (strcmp(argv[i], "--iters") == 0 && strcmp(argv[i + 1], "200") == 0)
            iters_default_seen = true;
        if (strcmp(argv[i], "--num-layers") == 0 && strcmp(argv[i + 1], "8") == 0)
            layers_default_seen = true;
    }
    HU_ASSERT_TRUE(batch_default_seen);
    HU_ASSERT_TRUE(iters_default_seen);
    HU_ASSERT_TRUE(layers_default_seen);
}

static void test_build_argv_explicit_hyperparams_win(void) {
    hu_lora_subprocess_config_t cfg;
    make_cfg(&cfg);
    cfg.batch_size = 16;
    cfg.iters = 500;
    cfg.lora_layers = 16;
    const char *argv[24];
    char buf[2048];
    size_t n = hu_lora_subprocess_build_argv(&cfg, argv, 24, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    bool batch_override_seen = false, iters_override_seen = false, layers_override_seen = false;
    for (size_t i = 0; i + 1 < n; i++) {
        if (strcmp(argv[i], "--batch-size") == 0 && strcmp(argv[i + 1], "16") == 0)
            batch_override_seen = true;
        if (strcmp(argv[i], "--iters") == 0 && strcmp(argv[i + 1], "500") == 0)
            iters_override_seen = true;
        if (strcmp(argv[i], "--num-layers") == 0 && strcmp(argv[i + 1], "16") == 0)
            layers_override_seen = true;
    }
    HU_ASSERT_TRUE(batch_override_seen);
    HU_ASSERT_TRUE(iters_override_seen);
    HU_ASSERT_TRUE(layers_override_seen);
}

static void test_build_argv_capacity_too_small_returns_zero(void) {
    hu_lora_subprocess_config_t cfg;
    make_cfg(&cfg);
    /* argv_cap=3 is below the minimum needed (we need ~17 entries
     * for the full command). The impl's preflight rejects <4. */
    const char *argv[3];
    char buf[2048];
    HU_ASSERT_EQ((int)hu_lora_subprocess_build_argv(&cfg, argv, 3, buf, sizeof(buf)), 0);
}

/* ── preflight ───────────────────────────────────────────────────────── */

static void test_preflight_null_cfg_false(void) {
    HU_ASSERT_TRUE(!hu_lora_subprocess_preflight_ok(NULL));
}

static void test_preflight_empty_base_model_false(void) {
    hu_lora_subprocess_config_t cfg;
    make_cfg(&cfg);
    cfg.base_model[0] = '\0';
    HU_ASSERT_TRUE(!hu_lora_subprocess_preflight_ok(&cfg));
}

static void test_preflight_missing_data_file_false(void) {
    hu_lora_subprocess_config_t cfg;
    make_cfg(&cfg);
    snprintf(cfg.data_jsonl_path, sizeof(cfg.data_jsonl_path),
             "/tmp/definitely-does-not-exist-%d.jsonl", (int)getpid());
    HU_ASSERT_TRUE(!hu_lora_subprocess_preflight_ok(&cfg));
}

/* ── end-to-end ──────────────────────────────────────────────────────── */

static void test_train_null_alloc_returns_invalid(void) {
    hu_lora_subprocess_config_t cfg;
    make_cfg(&cfg);
    HU_ASSERT_EQ((int)hu_lora_subprocess_train(NULL, &cfg), (int)HU_ERR_INVALID_ARGUMENT);
}

void run_lora_subprocess_tests(void) {
    HU_TEST_SUITE("lora_subprocess");
    HU_RUN_TEST(test_build_argv_happy_path_shape);
    HU_RUN_TEST(test_build_argv_null_inputs_return_zero);
    HU_RUN_TEST(test_build_argv_missing_required_field_returns_zero);
    HU_RUN_TEST(test_build_argv_zero_hyperparams_use_defaults);
    HU_RUN_TEST(test_build_argv_explicit_hyperparams_win);
    HU_RUN_TEST(test_build_argv_capacity_too_small_returns_zero);
    HU_RUN_TEST(test_preflight_null_cfg_false);
    HU_RUN_TEST(test_preflight_empty_base_model_false);
    HU_RUN_TEST(test_preflight_missing_data_file_false);
    HU_RUN_TEST(test_train_null_alloc_returns_invalid);
}
