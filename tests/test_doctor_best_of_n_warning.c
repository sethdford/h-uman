/*
 * US-7.7 AC-7.7.3 — doctor warning for cloud-provider + inference.best_of_n
 * misconfiguration.
 *
 * The decorator silently no-ops on non-llamacpp providers (eligibility gate
 * at the agent_turn dispatch site). Doctor is the operator-visible signal.
 */

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/doctor.h"
#include "test_framework.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static hu_allocator_t alloc(void) {
    return hu_system_allocator();
}

static void init_min_cfg(hu_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->default_temperature = 0.7;
    cfg->gateway.port = 8080;
}

static bool diag_contains(const hu_diag_item_t *items, size_t n, hu_diag_severity_t sev,
                          const char *needle) {
    for (size_t i = 0; i < n; ++i) {
        if (items[i].severity == sev && items[i].message &&
            strstr(items[i].message, needle) != NULL)
            return true;
    }
    return false;
}

static void free_items(hu_allocator_t *a, hu_diag_item_t *items, size_t n) {
    if (!items)
        return;
    for (size_t i = 0; i < n; ++i) {
        if (items[i].category)
            a->free(a->ctx, (void *)items[i].category, strlen(items[i].category) + 1);
        if (items[i].message)
            a->free(a->ctx, (void *)items[i].message, strlen(items[i].message) + 1);
    }
    /* The doctor allocates the item buffer up-front with a capacity that
     * may exceed `n`; the caller doesn't know `cap`. Best-effort: free a
     * generous block. The system allocator's free is a no-op on size in
     * this build, so the size argument is informational only. */
    a->free(a->ctx, items, sizeof(hu_diag_item_t) * n);
}

/* AC-7.7.3: cloud provider + inference.best_of_n >= 2 → WARN. */
static void test_doctor_warns_when_cloud_provider_has_best_of_n(void) {
    hu_allocator_t a = alloc();
    hu_config_t cfg;
    init_min_cfg(&cfg);
    cfg.default_provider = (char *)"openai"; /* cloud provider */
    cfg.inference.best_of_n = 4;
    cfg.inference.best_of_n_cost_cap_ms = 0;

    hu_diag_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_doctor_check_config_semantics(&a, &cfg, &items, &n), HU_OK);
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(diag_contains(items, n, HU_DIAG_WARN,
                                 "inference.best_of_n has no effect with cloud providers"));
    free_items(&a, items, n);
}

/* Negative: local provider (llamacpp) + best_of_n >= 2 → no warning. */
static void test_doctor_silent_when_local_provider(void) {
    hu_allocator_t a = alloc();
    hu_config_t cfg;
    init_min_cfg(&cfg);
    cfg.default_provider = (char *)"llamacpp"; /* local provider */
    cfg.inference.best_of_n = 4;

    hu_diag_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_doctor_check_config_semantics(&a, &cfg, &items, &n), HU_OK);
    HU_ASSERT_FALSE(diag_contains(items, n, HU_DIAG_WARN,
                                  "inference.best_of_n has no effect with cloud providers"));
    free_items(&a, items, n);
}

/* Negative: cloud provider + best_of_n disabled → no warning. */
static void test_doctor_silent_when_best_of_n_disabled(void) {
    hu_allocator_t a = alloc();
    hu_config_t cfg;
    init_min_cfg(&cfg);
    cfg.default_provider = (char *)"openai";
    cfg.inference.best_of_n = 1; /* disabled */

    hu_diag_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_doctor_check_config_semantics(&a, &cfg, &items, &n), HU_OK);
    HU_ASSERT_FALSE(diag_contains(items, n, HU_DIAG_WARN,
                                  "inference.best_of_n has no effect with cloud providers"));
    free_items(&a, items, n);

    /* And with best_of_n = 0 (also disabled). */
    init_min_cfg(&cfg);
    cfg.default_provider = (char *)"openai";
    cfg.inference.best_of_n = 0;
    items = NULL;
    n = 0;
    HU_ASSERT_EQ(hu_doctor_check_config_semantics(&a, &cfg, &items, &n), HU_OK);
    HU_ASSERT_FALSE(diag_contains(items, n, HU_DIAG_WARN,
                                  "inference.best_of_n has no effect with cloud providers"));
    free_items(&a, items, n);
}

/* Coexistence with US-7.3 warning: cloud + lora_adapter_path + best_of_n
 * both fire independently and don't interfere. */
static void test_doctor_warnings_coexist(void) {
    hu_allocator_t a = alloc();
    hu_config_t cfg;
    init_min_cfg(&cfg);
    cfg.default_provider = (char *)"openai";
    cfg.inference.best_of_n = 4;
    cfg.personalization.lora_adapter_path = (char *)"/tmp/test.lora";

    hu_diag_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_doctor_check_config_semantics(&a, &cfg, &items, &n), HU_OK);
    HU_ASSERT_TRUE(diag_contains(items, n, HU_DIAG_WARN, "personalization.lora_adapter_path"));
    HU_ASSERT_TRUE(diag_contains(items, n, HU_DIAG_WARN,
                                 "inference.best_of_n has no effect with cloud providers"));
    free_items(&a, items, n);
}

/* 2026-05-25 reactive-iMessage recovery — privacy-thesis alignment check.
 *
 * Operator has a local provider (mlx_local / llamacpp / ollama / apple)
 * registered in providers[] but default_provider points at a cloud
 * provider. The local model is configured AND probably running, but
 * unused. Per CLAUDE.md product thesis: "Privacy by architecture, not
 * by settings." Surface as INFO/WARN so the operator can flip the switch.
 *
 * Real-world instance: gemma-4-31b-seth-v3-fused serving locally at
 * 11.9 tok/s, providers[] has mlx_local, default_provider="gemini". */
static void test_doctor_warns_when_local_provider_configured_but_cloud_default(void) {
    hu_allocator_t a = alloc();
    hu_config_t cfg;
    init_min_cfg(&cfg);
    cfg.default_provider = (char *)"gemini";
    hu_provider_entry_t providers[1];
    memset(providers, 0, sizeof(providers));
    providers[0].name = (char *)"mlx_local";
    providers[0].base_url = (char *)"http://127.0.0.1:8741/v1";
    cfg.providers = providers;
    cfg.providers_len = 1;

    hu_diag_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_doctor_check_config_semantics(&a, &cfg, &items, &n), HU_OK);
    HU_ASSERT_TRUE(diag_contains(items, n, HU_DIAG_WARN, "local provider 'mlx_local'"));
    HU_ASSERT_TRUE(diag_contains(items, n, HU_DIAG_WARN, "default_provider='gemini'"));
    free_items(&a, items, n);
}

/* Negative: local provider IS the default → no warning. */
static void test_doctor_silent_when_local_provider_is_default(void) {
    hu_allocator_t a = alloc();
    hu_config_t cfg;
    init_min_cfg(&cfg);
    cfg.default_provider = (char *)"mlx_local"; /* already using local */
    hu_provider_entry_t providers[1];
    memset(providers, 0, sizeof(providers));
    providers[0].name = (char *)"mlx_local";
    cfg.providers = providers;
    cfg.providers_len = 1;

    hu_diag_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_doctor_check_config_semantics(&a, &cfg, &items, &n), HU_OK);
    HU_ASSERT_FALSE(diag_contains(items, n, HU_DIAG_WARN, "local provider"));
    free_items(&a, items, n);
}

/* Negative: cloud default with no local providers registered → no warning. */
static void test_doctor_silent_when_no_local_provider_registered(void) {
    hu_allocator_t a = alloc();
    hu_config_t cfg;
    init_min_cfg(&cfg);
    cfg.default_provider = (char *)"openai";
    /* No providers[] entry at all */
    cfg.providers = NULL;
    cfg.providers_len = 0;

    hu_diag_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_doctor_check_config_semantics(&a, &cfg, &items, &n), HU_OK);
    HU_ASSERT_FALSE(diag_contains(items, n, HU_DIAG_WARN, "local provider"));
    free_items(&a, items, n);
}

/* All four recognized local provider names trigger the warning when
 * default_provider is cloud. Each one independently. */
static void test_doctor_warns_for_each_recognized_local_provider_name(void) {
    const char *local_names[] = {"mlx_local", "llamacpp", "ollama", "apple"};
    for (size_t i = 0; i < sizeof(local_names) / sizeof(local_names[0]); i++) {
        hu_allocator_t a = alloc();
        hu_config_t cfg;
        init_min_cfg(&cfg);
        cfg.default_provider = (char *)"openai"; /* cloud */
        hu_provider_entry_t providers[1];
        memset(providers, 0, sizeof(providers));
        providers[0].name = (char *)local_names[i];
        cfg.providers = providers;
        cfg.providers_len = 1;

        hu_diag_item_t *items = NULL;
        size_t n = 0;
        HU_ASSERT_EQ(hu_doctor_check_config_semantics(&a, &cfg, &items, &n), HU_OK);
        HU_ASSERT_TRUE(diag_contains(items, n, HU_DIAG_WARN, "local provider"));
        free_items(&a, items, n);
    }
}

void run_doctor_best_of_n_warning_tests(void) {
    HU_TEST_SUITE("DoctorBestOfNWarning (US-7.7 AC-7.7.3)");
    HU_RUN_TEST(test_doctor_warns_when_cloud_provider_has_best_of_n);
    HU_RUN_TEST(test_doctor_silent_when_local_provider);
    HU_RUN_TEST(test_doctor_silent_when_best_of_n_disabled);
    HU_RUN_TEST(test_doctor_warnings_coexist);
    HU_RUN_TEST(test_doctor_warns_when_local_provider_configured_but_cloud_default);
    HU_RUN_TEST(test_doctor_silent_when_local_provider_is_default);
    HU_RUN_TEST(test_doctor_silent_when_no_local_provider_registered);
    HU_RUN_TEST(test_doctor_warns_for_each_recognized_local_provider_name);
}
