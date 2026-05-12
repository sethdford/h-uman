/* Phase 1 (RL SOTA) — LoRA hot-swap integration test.
 *
 * GATED by env vars HU_HAVE_GEMMA_GGUF=1 + HU_HAVE_GEMMA_LORA_FIXTURE=<path>.
 * When either is unset, the test SKIP's so default CI/local runs stay green
 * without requiring a 2.5 GB Gemma GGUF + a perturbing LoRA on disk.
 *
 * The test:
 *   1. Loads Gemma-3-4B-it base model via hu_llamacpp_provider_create
 *   2. Runs a chat call -> output_a
 *   3. Calls vtable->load_adapter(<fixture>)
 *   4. Asserts vtable->active_adapter() returns the id we just loaded
 *   5. Runs the SAME chat call -> output_b
 *   6. Asserts output_a != output_b (the LoRA must perturb output)
 *   7. Calls vtable->unload_adapter()
 *   8. Runs the SAME chat call -> output_c
 *   9. Asserts output_a == output_c (baseline restored)
 *
 * Step 9 is the post-implementation pin for Task 8's KV-cache reset
 * on adapter swap. Without llama_memory_clear + hu_llamacpp_kvcache_reset
 * in load_adapter / unload_adapter, residual KV from chat A would leak
 * into B and C and step 9 would flake.
 *
 * The fixture is supplied externally because constructing a known-perturbing
 * GGUF LoRA from scratch in C is out of scope. Any small published Gemma-3
 * LoRA on HuggingFace works — we only assert "output changes", not that it
 * matches a specific string.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/providers/llamacpp.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gemma_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home) return 0;
    int n = snprintf(out, cap, "%s/.human/models/gemma-3-it-4B-Q4_K_M.gguf", home);
    if (n < 0 || (size_t)n >= cap) return 0;
    FILE *f = fopen(out, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static char *do_chat(hu_provider_t *p, hu_allocator_t *alloc,
                     const char *sys, const char *msg) {
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = p->vtable->chat_with_system(
        p->ctx, alloc, sys, strlen(sys), msg, strlen(msg),
        "gemma-3-4b-it", strlen("gemma-3-4b-it"), 0.0, &out, &out_len);
    return (err == HU_OK) ? out : NULL;
}

static void test_lora_hot_swap_changes_output_then_unload_restores(void) {
    const char *fixture = getenv("HU_HAVE_GEMMA_LORA_FIXTURE");
    const char *gguf_flag = getenv("HU_HAVE_GEMMA_GGUF");
    if (!fixture || !gguf_flag) {
        fprintf(stderr, "[skip] HU_HAVE_GEMMA_LORA_FIXTURE / HU_HAVE_GEMMA_GGUF unset\n");
        return;
    }

    char gguf[1024];
    if (!gemma_path(gguf, sizeof(gguf))) {
        fprintf(stderr, "[skip] gemma gguf missing at ~/.human/models/\n");
        return;
    }

    hu_allocator_t alloc = hu_system_allocator();
    hu_llamacpp_config_t cfg = {
        .model_path = gguf,
        .context_size = 2048,
        .threads = 4,
        .use_gpu = true,
        .n_gpu_layers = -1,
    };
    hu_provider_t provider = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&alloc, &cfg, &provider), HU_OK);
    HU_ASSERT_NOT_NULL(provider.vtable);
    HU_ASSERT_NOT_NULL(provider.vtable->load_adapter);
    HU_ASSERT_NOT_NULL(provider.vtable->unload_adapter);
    HU_ASSERT_NOT_NULL(provider.vtable->active_adapter);

    const char *sys = "You are a chatbot.";
    const char *msg = "Say a short greeting.";

    char *output_a = do_chat(&provider, &alloc, sys, msg);
    HU_ASSERT_NOT_NULL(output_a);

    const hu_lora_adapter_spec_t spec = {
        .path = fixture, .path_len = strlen(fixture),
        .id = "test-lora", .id_len = strlen("test-lora"),
        .alloc = &alloc,
    };
    HU_ASSERT_EQ(provider.vtable->load_adapter(provider.ctx, &spec,
                                               HU_LORA_APPLY_MODE_REPLACE),
                 HU_OK);
    const char *active = provider.vtable->active_adapter(provider.ctx);
    HU_ASSERT_NOT_NULL(active);
    HU_ASSERT_STR_EQ(active, "test-lora");

    char *output_b = do_chat(&provider, &alloc, sys, msg);
    HU_ASSERT_NOT_NULL(output_b);

    /* The LoRA must produce different output. Even a small adapter on
     * Gemma-3-4B perturbs greedy decoding within a few tokens. */
    HU_ASSERT_FALSE(strcmp(output_a, output_b) == 0);

    HU_ASSERT_EQ(provider.vtable->unload_adapter(provider.ctx, "test-lora",
                                                 strlen("test-lora")),
                 HU_OK);

    char *output_c = do_chat(&provider, &alloc, sys, msg);
    HU_ASSERT_NOT_NULL(output_c);
    /* Output after unload must match the base output. This only holds
     * because Task 8's load/unload paths call llama_memory_clear +
     * hu_llamacpp_kvcache_reset. */
    HU_ASSERT_STR_EQ(output_a, output_c);

    free(output_a);
    free(output_b);
    free(output_c);
    if (provider.vtable->deinit) provider.vtable->deinit(provider.ctx, &alloc);
}

void run_llamacpp_lora_hotswap_tests(void) {
    HU_RUN_TEST(test_lora_hot_swap_changes_output_then_unload_restores);
}
