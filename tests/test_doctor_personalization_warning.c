/* US-7.3 (INS-B) — doctor-side honesty gate.
 *
 * `hu_doctor_check_config_semantics` must emit a WARN line containing
 * "[WARN] personalization.lora_adapter_path is set but the active "
 * "provider does not support adapters" whenever:
 *
 *   personalization.lora_adapter_path is set AND
 *   hu_config_provider_requires_api_key(default_provider) returns true
 *
 * It must stay silent for:
 *   - personalization.lora_adapter_path unset/empty
 *   - default_provider is a local provider (e.g. llamacpp) — even if
 *     lora_adapter_path is set
 *
 * The literal-string contract is pinned here. Drift between this file
 * and src/doctor.c is intentional: AC-7.3.2 verifies the surface,
 * not the heuristic.
 */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_LORA_DOCTOR_WARN_LITERAL                                            \
    "[WARN] personalization.lora_adapter_path is set but the active provider " \
    "does not support adapters"

static void free_semantics_items(hu_allocator_t *alloc, hu_diag_item_t *items, size_t count) {
    if (!items)
        return;
    for (size_t i = 0; i < count; i++) {
        if (items[i].category)
            alloc->free(alloc->ctx, (void *)items[i].category, strlen(items[i].category) + 1);
        if (items[i].message)
            alloc->free(alloc->ctx, (void *)items[i].message, strlen(items[i].message) + 1);
    }
    alloc->free(alloc->ctx, items, sizeof(hu_diag_item_t) * count);
}

static bool diag_has_warn_with_substr(const hu_diag_item_t *items, size_t count,
                                      const char *needle) {
    for (size_t i = 0; i < count; i++) {
        if (items[i].severity != HU_DIAG_WARN)
            continue;
        if (items[i].message && strstr(items[i].message, needle))
            return true;
    }
    return false;
}

static bool diag_has_substr(const hu_diag_item_t *items, size_t count, const char *needle) {
    for (size_t i = 0; i < count; i++) {
        if (items[i].message && strstr(items[i].message, needle))
            return true;
    }
    return false;
}

/* Build a minimal hu_config_t with stack-rooted strings. The doctor
 * never frees these pointers — it strdups what it needs into its own
 * diag items. */
static void build_config(hu_config_t *cfg, char *provider, char *lora_path) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->default_provider = provider;
    cfg->default_temperature = 0.7;
    cfg->gateway.port = 3000;
    cfg->personalization.enabled = (lora_path != NULL);
    cfg->personalization.lora_adapter_path = lora_path;
}

/* AC-7.3.2 — cloud provider + lora_adapter_path → WARN line. */
static void test_doctor_warns_when_cloud_provider_has_lora_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg;
    char provider[] = "openai";
    char lora[] = "/tmp/persona-default.lora";
    build_config(&cfg, provider, lora);

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_TRUE(count > 0);
    HU_ASSERT_TRUE(diag_has_warn_with_substr(items, count, HU_LORA_DOCTOR_WARN_LITERAL));

    free_semantics_items(&alloc, items, count);
}

/* AC-7.3.3 — lora_adapter_path unset → silent. */
static void test_doctor_silent_when_no_adapter_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg;
    char provider[] = "openai";
    build_config(&cfg, provider, NULL);

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_FALSE(diag_has_substr(items, count, HU_LORA_DOCTOR_WARN_LITERAL));

    free_semantics_items(&alloc, items, count);
}

/* AC-7.3.3 secondary — empty (non-NULL) string also silent. */
static void test_doctor_silent_when_adapter_path_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg;
    char provider[] = "openai";
    char empty[] = "";
    build_config(&cfg, provider, empty);

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_FALSE(diag_has_substr(items, count, HU_LORA_DOCTOR_WARN_LITERAL));

    free_semantics_items(&alloc, items, count);
}

/* AC-7.3.4 — local provider + lora_adapter_path → silent. */
static void test_doctor_silent_when_local_provider(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg;
    char provider[] = "llamacpp";
    char lora[] = "/tmp/persona-default.lora";
    build_config(&cfg, provider, lora);

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_FALSE(diag_has_substr(items, count, HU_LORA_DOCTOR_WARN_LITERAL));

    free_semantics_items(&alloc, items, count);
}

void run_doctor_personalization_warning_tests(void) {
    HU_TEST_SUITE("Doctor Personalization Warning");
    HU_RUN_TEST(test_doctor_warns_when_cloud_provider_has_lora_path);
    HU_RUN_TEST(test_doctor_silent_when_no_adapter_path);
    HU_RUN_TEST(test_doctor_silent_when_adapter_path_empty);
    HU_RUN_TEST(test_doctor_silent_when_local_provider);
}
