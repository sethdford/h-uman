/*
 * Phase 1 (RL SOTA) — factory entry-point wiring tests.
 *
 * Verifies that hu_provider_create_from_entry forwards every llamacpp
 * tuning field (context_size / threads / use_gpu / n_gpu_layers) plus
 * the model_path (via base_url) into the hu_llamacpp_config_t handed
 * to hu_llamacpp_provider_create.
 *
 * These tests are isolated from libllama: the test hook captures the
 * config struct the factory built and we assert on it directly. No
 * model is loaded, no Metal kernels are dispatched.
 */

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#include "human/providers/llamacpp.h"
#include "test_framework.h"

#include <string.h>

static hu_allocator_t alloc(void) {
    return hu_system_allocator();
}

static void test_factory_forwards_full_llamacpp_config(void) {
    hu_llamacpp_factory_reset_for_test();

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/some-model.gguf",
        .context_size = 8192,
        .threads = 6,
        .use_gpu = true,
        .n_gpu_layers = 99,
    };
    hu_provider_t prov = {0};

    hu_error_t r = hu_provider_create_from_entry(&a, &entry, &prov);
    HU_ASSERT_EQ(r, HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_NOT_NULL(captured->model_path);
    HU_ASSERT_STR_EQ(captured->model_path, "/tmp/some-model.gguf");
    HU_ASSERT_EQ(captured->context_size, (size_t)8192);
    HU_ASSERT_EQ(captured->threads, 6);
    HU_ASSERT_TRUE(captured->use_gpu);
    HU_ASSERT_EQ(captured->n_gpu_layers, 99);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_llamacpp_dotted_alias(void) {
    /* Both "llamacpp" and "llama.cpp" must dispatch to the same
     * builder — the dotted form is the canonical config-file spelling
     * but our internal name table uses the dotless form. */
    hu_llamacpp_factory_reset_for_test();

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llama.cpp",
        .base_url = (char *)"/tmp/dotted.gguf",
        .context_size = 4096,
    };
    hu_provider_t prov = {0};

    hu_error_t r = hu_provider_create_from_entry(&a, &entry, &prov);
    HU_ASSERT_EQ(r, HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_STR_EQ(captured->model_path, "/tmp/dotted.gguf");
    HU_ASSERT_EQ(captured->context_size, (size_t)4096);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_zero_fields_pass_through(void) {
    /* When the JSON config omits the tuning fields, zero must reach
     * the llamacpp config (which interprets zero as "use llama.cpp's
     * default"). The capture hook lets us prove that. */
    hu_llamacpp_factory_reset_for_test();

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/defaults.gguf",
    };
    hu_provider_t prov = {0};

    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_EQ(captured->context_size, (size_t)0);
    HU_ASSERT_EQ(captured->threads, 0);
    HU_ASSERT_FALSE(captured->use_gpu);
    HU_ASSERT_EQ(captured->n_gpu_layers, 0);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_non_llamacpp_skips_capture(void) {
    /* A non-llamacpp entry must not touch the llamacpp capture state.
     * Use ollama because it doesn't require an api_key to construct. */
    hu_llamacpp_factory_reset_for_test();

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"ollama",
        .base_url = (char *)"http://localhost:11434",
    };
    hu_provider_t prov = {0};

    hu_error_t r = hu_provider_create_from_entry(&a, &entry, &prov);
    HU_ASSERT_EQ(r, HU_OK);
    HU_ASSERT_NULL(hu_llamacpp_factory_last_config());

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
}

static void test_factory_rejects_null_args(void) {
    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {.name = (char *)"llamacpp"};
    hu_provider_t prov = {0};

    HU_ASSERT_EQ(hu_provider_create_from_entry(NULL, &entry, &prov), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, NULL, &prov), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, NULL), HU_ERR_INVALID_ARGUMENT);

    hu_provider_entry_t no_name = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &no_name, &prov), HU_ERR_INVALID_ARGUMENT);
}

/* Phase 1b — HU_LLAMACPP_KV_QUANT env-var bridge contract.
 *
 * The factory reads HU_LLAMACPP_KV_QUANT once per provider creation
 * and threads the parsed value into hu_llamacpp_config_t.kv_quant.
 * Default (env unset) preserves FP16; valid values set Q8_0 / Q4_0;
 * unrecognized values silently fall back to FP16 (parse function
 * absorbs the typo — operator gets recognized=false signal at the
 * parse layer, not here at the factory). */

static void test_factory_kv_quant_env_unset_defaults_to_fp16(void) {
    hu_llamacpp_factory_reset_for_test();
    unsetenv("HU_LLAMACPP_KV_QUANT");

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/no-env.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_EQ((int)captured->kv_quant, (int)HU_KV_QUANT_FP16);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_kv_quant_env_q8_0_sets_quant(void) {
    hu_llamacpp_factory_reset_for_test();
    setenv("HU_LLAMACPP_KV_QUANT", "q8_0", 1);

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/q8.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_EQ((int)captured->kv_quant, (int)HU_KV_QUANT_Q8_0);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    unsetenv("HU_LLAMACPP_KV_QUANT");
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_kv_quant_env_q4_0_sets_quant(void) {
    hu_llamacpp_factory_reset_for_test();
    setenv("HU_LLAMACPP_KV_QUANT", "q4_0", 1);

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/q4.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_EQ((int)captured->kv_quant, (int)HU_KV_QUANT_Q4_0);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    unsetenv("HU_LLAMACPP_KV_QUANT");
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_kv_quant_env_unrecognized_falls_back_to_fp16(void) {
    /* Adversarial: typo in env value must not silently quantize. The
     * parse function returns FP16 + recognized=false for unknown input
     * (pinned by test_llamacpp_kv_quant); the factory inherits that
     * behavior. */
    hu_llamacpp_factory_reset_for_test();
    setenv("HU_LLAMACPP_KV_QUANT", "q3_0", 1); /* not a supported variant */

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/typo.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_EQ((int)captured->kv_quant, (int)HU_KV_QUANT_FP16);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    unsetenv("HU_LLAMACPP_KV_QUANT");
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_kv_quant_env_empty_string_treated_as_unset(void) {
    /* Empty HU_LLAMACPP_KV_QUANT="" must NOT be interpreted as Q8 or
     * Q4 — it's the same as unset. Pinned because the parse function
     * returns FP16 + recognized=false for empty input, and the factory
     * must respect "unset means leave default" semantics. */
    hu_llamacpp_factory_reset_for_test();
    setenv("HU_LLAMACPP_KV_QUANT", "", 1);

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/empty.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_EQ((int)captured->kv_quant, (int)HU_KV_QUANT_FP16);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    unsetenv("HU_LLAMACPP_KV_QUANT");
    hu_llamacpp_factory_reset_for_test();
}

/* Phase 3b — HU_LLAMACPP_DRAFT_MODEL env-var bridge.
 *
 * Three optional env vars: HU_LLAMACPP_DRAFT_MODEL (path),
 * HU_LLAMACPP_DRAFT_MIN_P (float), HU_LLAMACPP_DRAFT_MAX_TOKENS (int).
 * All silently ignored if unset; unparseable numerics fall back to 0
 * so a typo doesn't break the factory. */

static void test_factory_draft_model_env_unset_leaves_null(void) {
    hu_llamacpp_factory_reset_for_test();
    unsetenv("HU_LLAMACPP_DRAFT_MODEL");
    unsetenv("HU_LLAMACPP_DRAFT_MIN_P");
    unsetenv("HU_LLAMACPP_DRAFT_MAX_TOKENS");

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/no-draft.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_TRUE(captured->draft_model_path == NULL);
    HU_ASSERT_EQ((int)(captured->draft_min_p * 1000), 0);
    HU_ASSERT_EQ(captured->draft_max_tokens, 0);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_draft_model_env_sets_path(void) {
    hu_llamacpp_factory_reset_for_test();
    setenv("HU_LLAMACPP_DRAFT_MODEL", "/tmp/draft-270m.gguf", 1);

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/target-31b.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_NOT_NULL(captured->draft_model_path);
    HU_ASSERT_STR_EQ(captured->draft_model_path, "/tmp/draft-270m.gguf");

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    unsetenv("HU_LLAMACPP_DRAFT_MODEL");
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_draft_min_p_env_sets_threshold(void) {
    hu_llamacpp_factory_reset_for_test();
    setenv("HU_LLAMACPP_DRAFT_MIN_P", "0.05", 1);

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/min-p.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    /* 0.05 -> 50 via *1000 (avoids float equality fragility). */
    HU_ASSERT_EQ((int)(captured->draft_min_p * 1000.0f + 0.5f), 50);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    unsetenv("HU_LLAMACPP_DRAFT_MIN_P");
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_draft_max_tokens_env_sets_value(void) {
    hu_llamacpp_factory_reset_for_test();
    setenv("HU_LLAMACPP_DRAFT_MAX_TOKENS", "7", 1);

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/max-tokens.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_EQ(captured->draft_max_tokens, 7);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    unsetenv("HU_LLAMACPP_DRAFT_MAX_TOKENS");
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_draft_env_invalid_numerics_ignored(void) {
    /* Adversarial: garbage env values must not break the factory.
     * Min-p out of [0,1] and max-tokens out of (0,64) silently fall
     * back to 0 (upstream default) — same friendly-to-typos posture
     * as KV quant. */
    hu_llamacpp_factory_reset_for_test();
    setenv("HU_LLAMACPP_DRAFT_MIN_P", "not-a-number", 1);
    setenv("HU_LLAMACPP_DRAFT_MAX_TOKENS", "9999", 1); /* out of range */

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/garbage-env.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_EQ((int)(captured->draft_min_p * 1000), 0);
    HU_ASSERT_EQ(captured->draft_max_tokens, 0);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    unsetenv("HU_LLAMACPP_DRAFT_MIN_P");
    unsetenv("HU_LLAMACPP_DRAFT_MAX_TOKENS");
    hu_llamacpp_factory_reset_for_test();
}

/* Phase 4 — HU_LLAMACPP_FLASH_ATTN env-var. The factory defaults
 * flash_attn=true (FA is table-stakes on Mac Metal builds). Operators
 * disable for debugging by setting the env to "off" / "0" / "false".
 * Anything else (including typos and "on"/"1"/"yes"/empty) keeps it
 * enabled — same friendly-to-typos posture as the other env bridges. */

static void test_factory_flash_attn_defaults_to_true(void) {
    hu_llamacpp_factory_reset_for_test();
    unsetenv("HU_LLAMACPP_FLASH_ATTN");

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/fa-default.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_TRUE(captured->flash_attn);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_flash_attn_env_off_disables(void) {
    hu_llamacpp_factory_reset_for_test();
    setenv("HU_LLAMACPP_FLASH_ATTN", "off", 1);

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/fa-off.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_FALSE(captured->flash_attn);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    unsetenv("HU_LLAMACPP_FLASH_ATTN");
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_flash_attn_env_zero_and_false_also_disable(void) {
    /* Pin all three off-tokens because operators conflate them
     * across env conventions. */
    const char *off_tokens[] = {"0", "false"};
    for (size_t i = 0; i < sizeof(off_tokens) / sizeof(off_tokens[0]); i++) {
        hu_llamacpp_factory_reset_for_test();
        setenv("HU_LLAMACPP_FLASH_ATTN", off_tokens[i], 1);

        hu_allocator_t a = alloc();
        hu_provider_entry_t entry = {
            .name = (char *)"llamacpp",
            .base_url = (char *)"/tmp/fa-token.gguf",
        };
        hu_provider_t prov = {0};
        HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

        const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
        HU_ASSERT_NOT_NULL(captured);
        HU_ASSERT_FALSE(captured->flash_attn);

        if (prov.vtable && prov.vtable->deinit)
            prov.vtable->deinit(prov.ctx, &a);
        unsetenv("HU_LLAMACPP_FLASH_ATTN");
        hu_llamacpp_factory_reset_for_test();
    }
}

static void test_factory_flash_attn_env_unrecognized_keeps_default(void) {
    /* Adversarial: typos and "on"-flavored values must not silently
     * flip the default. The default is true; only the exact off-tokens
     * disable. */
    const char *keep_tokens[] = {"on", "1", "yes", "true", "ON", "typo"};
    for (size_t i = 0; i < sizeof(keep_tokens) / sizeof(keep_tokens[0]); i++) {
        hu_llamacpp_factory_reset_for_test();
        setenv("HU_LLAMACPP_FLASH_ATTN", keep_tokens[i], 1);

        hu_allocator_t a = alloc();
        hu_provider_entry_t entry = {
            .name = (char *)"llamacpp",
            .base_url = (char *)"/tmp/fa-keep.gguf",
        };
        hu_provider_t prov = {0};
        HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

        const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
        HU_ASSERT_NOT_NULL(captured);
        HU_ASSERT_TRUE(captured->flash_attn);

        if (prov.vtable && prov.vtable->deinit)
            prov.vtable->deinit(prov.ctx, &a);
        unsetenv("HU_LLAMACPP_FLASH_ATTN");
        hu_llamacpp_factory_reset_for_test();
    }
}

/* Phase 2b.2 — HU_LLAMACPP_KVCACHE_SKIP_DECODE env-var bridge.
 *
 * STRICTER opt-in than the other env bridges: only "1" / "on" / "true"
 * enables. Typos and "yes"/"enable"/etc. keep the SAFE default OFF
 * because mis-enabling this can silently corrupt KV in real linked-
 * libllama builds. The defensive posture is intentional, not paranoid:
 * Phase 2b only landed in main because the silent-corruption bug it
 * fixed was undetectable under the test preset. */

static void test_factory_kvcache_skip_decode_env_unset_defaults_to_off(void) {
    hu_llamacpp_factory_reset_for_test();
    unsetenv("HU_LLAMACPP_KVCACHE_SKIP_DECODE");

    hu_allocator_t a = alloc();
    hu_provider_entry_t entry = {
        .name = (char *)"llamacpp",
        .base_url = (char *)"/tmp/safe-default.gguf",
    };
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

    const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
    HU_ASSERT_NOT_NULL(captured);
    HU_ASSERT_FALSE(captured->kvcache_skip_decode);

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &a);
    hu_llamacpp_factory_reset_for_test();
}

static void test_factory_kvcache_skip_decode_env_on_tokens_enable(void) {
    /* The three accepted on-tokens. Each enables the opt-in. */
    const char *on_tokens[] = {"1", "on", "true"};
    for (size_t i = 0; i < sizeof(on_tokens) / sizeof(on_tokens[0]); i++) {
        hu_llamacpp_factory_reset_for_test();
        setenv("HU_LLAMACPP_KVCACHE_SKIP_DECODE", on_tokens[i], 1);

        hu_allocator_t a = alloc();
        hu_provider_entry_t entry = {
            .name = (char *)"llamacpp",
            .base_url = (char *)"/tmp/skip-on.gguf",
        };
        hu_provider_t prov = {0};
        HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

        const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
        HU_ASSERT_NOT_NULL(captured);
        HU_ASSERT_TRUE(captured->kvcache_skip_decode);

        if (prov.vtable && prov.vtable->deinit)
            prov.vtable->deinit(prov.ctx, &a);
        unsetenv("HU_LLAMACPP_KVCACHE_SKIP_DECODE");
        hu_llamacpp_factory_reset_for_test();
    }
}

static void test_factory_kvcache_skip_decode_env_typo_keeps_safe_default(void) {
    /* Adversarial: typo or "yes"/"enable" must NOT silently enable.
     * The strict-token posture is what separates this bridge from the
     * friendly-to-typos posture of kv_quant / flash_attn — mis-enabling
     * silently corrupts KV; mis-disabling just leaves perf on the table. */
    const char *off_tokens[] = {"yes", "enable", "TRUE", "ON", "y", "off", "0", "garbage"};
    for (size_t i = 0; i < sizeof(off_tokens) / sizeof(off_tokens[0]); i++) {
        hu_llamacpp_factory_reset_for_test();
        setenv("HU_LLAMACPP_KVCACHE_SKIP_DECODE", off_tokens[i], 1);

        hu_allocator_t a = alloc();
        hu_provider_entry_t entry = {
            .name = (char *)"llamacpp",
            .base_url = (char *)"/tmp/skip-strict.gguf",
        };
        hu_provider_t prov = {0};
        HU_ASSERT_EQ(hu_provider_create_from_entry(&a, &entry, &prov), HU_OK);

        const hu_llamacpp_config_t *captured = hu_llamacpp_factory_last_config();
        HU_ASSERT_NOT_NULL(captured);
        HU_ASSERT_FALSE(captured->kvcache_skip_decode);

        if (prov.vtable && prov.vtable->deinit)
            prov.vtable->deinit(prov.ctx, &a);
        unsetenv("HU_LLAMACPP_KVCACHE_SKIP_DECODE");
        hu_llamacpp_factory_reset_for_test();
    }
}

void run_llamacpp_factory_config_tests(void) {
    HU_RUN_TEST(test_factory_forwards_full_llamacpp_config);
    HU_RUN_TEST(test_factory_llamacpp_dotted_alias);
    HU_RUN_TEST(test_factory_zero_fields_pass_through);
    HU_RUN_TEST(test_factory_non_llamacpp_skips_capture);
    HU_RUN_TEST(test_factory_rejects_null_args);
    HU_RUN_TEST(test_factory_kv_quant_env_unset_defaults_to_fp16);
    HU_RUN_TEST(test_factory_kv_quant_env_q8_0_sets_quant);
    HU_RUN_TEST(test_factory_kv_quant_env_q4_0_sets_quant);
    HU_RUN_TEST(test_factory_kv_quant_env_unrecognized_falls_back_to_fp16);
    HU_RUN_TEST(test_factory_kv_quant_env_empty_string_treated_as_unset);
    HU_RUN_TEST(test_factory_draft_model_env_unset_leaves_null);
    HU_RUN_TEST(test_factory_draft_model_env_sets_path);
    HU_RUN_TEST(test_factory_draft_min_p_env_sets_threshold);
    HU_RUN_TEST(test_factory_draft_max_tokens_env_sets_value);
    HU_RUN_TEST(test_factory_draft_env_invalid_numerics_ignored);
    HU_RUN_TEST(test_factory_flash_attn_defaults_to_true);
    HU_RUN_TEST(test_factory_flash_attn_env_off_disables);
    HU_RUN_TEST(test_factory_flash_attn_env_zero_and_false_also_disable);
    HU_RUN_TEST(test_factory_flash_attn_env_unrecognized_keeps_default);
    HU_RUN_TEST(test_factory_kvcache_skip_decode_env_unset_defaults_to_off);
    HU_RUN_TEST(test_factory_kvcache_skip_decode_env_on_tokens_enable);
    HU_RUN_TEST(test_factory_kvcache_skip_decode_env_typo_keeps_safe_default);
}
