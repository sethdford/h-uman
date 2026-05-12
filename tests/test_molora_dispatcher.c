/* SOTA-2026 init-02 — MoLoRA dispatcher unit tests.
 *
 * Pins the contract spelled out in `include/human/agent/molora_dispatcher.h`:
 *   1. NULL provider / NULL allocator ⇒ HU_ERR_INVALID_ARGUMENT.
 *   2. Cloud provider (load_adapter == NULL) ⇒ HU_ERR_NOT_SUPPORTED; STACK
 *      is NEVER attempted. Mirrors the M3 daemon cloud-fallthrough pattern.
 *   3. mlx_qwen3 + overlay with `lora_adapter_path` ⇒ both REPLACE base and
 *      STACK channel expert run; result diagnostics reflect both.
 *   4. mlx_qwen3 + overlay without `lora_adapter_path` ⇒ REPLACE only; no
 *      STACK attempt is recorded.
 *   5. Provider that supports REPLACE but rejects STACK with NOT_SUPPORTED
 *      ⇒ result reports `channel_expert_skipped = true`, function still
 *      returns HU_OK so the agent stays on the base adapter.
 *
 * Note: huml and llamacpp are not exercised here — both have a runtime
 * dependency on their respective build options (`HU_ENABLE_HUML`,
 * `HU_ENABLE_LLAMACPP`). The contract for those providers is tested in
 * their own suites; here we synthesize a fake provider that mimics their
 * REPLACE-only behaviour to keep the dispatcher test deterministic and
 * independent of build flags.
 */

#include "human/agent/molora_dispatcher.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/lora.h"
#include "human/persona.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#include "human/providers/mlx_qwen3.h"
#include "test_framework.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────
 * Fakes
 * ────────────────────────────────────────────────────────────────── */

/* A provider that honors REPLACE but explicitly returns NOT_SUPPORTED
 * for STACK — exactly the huml / llamacpp contract. The dispatcher must
 * still return HU_OK and flag `channel_expert_skipped`. */
typedef struct replace_only_ctx {
    char     *active_id;
    int       replace_calls;
    int       stack_calls;
} replace_only_ctx_t;

static hu_error_t replace_only_load_adapter(void *ctx, const hu_lora_adapter_spec_t *spec,
                                            hu_lora_apply_mode_t mode) {
    replace_only_ctx_t *c = (replace_only_ctx_t *)ctx;
    if (mode == HU_LORA_APPLY_MODE_STACK) {
        c->stack_calls++;
        return HU_ERR_NOT_SUPPORTED;
    }
    c->replace_calls++;
    if (c->active_id) {
        hu_allocator_t *a = spec->alloc;
        a->free(a->ctx, c->active_id, strlen(c->active_id) + 1);
        c->active_id = NULL;
    }
    c->active_id = spec->alloc->alloc(spec->alloc->ctx, spec->id_len + 1);
    if (!c->active_id)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(c->active_id, spec->id, spec->id_len);
    c->active_id[spec->id_len] = '\0';
    return HU_OK;
}

static const char *replace_only_active_adapter(void *ctx) {
    replace_only_ctx_t *c = (replace_only_ctx_t *)ctx;
    return c->active_id;
}

static const char *replace_only_get_name(void *ctx) {
    (void)ctx;
    return "replace_only_fake";
}

static void replace_only_deinit(void *ctx, hu_allocator_t *alloc) {
    replace_only_ctx_t *c = (replace_only_ctx_t *)ctx;
    if (c->active_id)
        alloc->free(alloc->ctx, c->active_id, strlen(c->active_id) + 1);
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_provider_vtable_t kReplaceOnlyVtable = {
    .get_name       = replace_only_get_name,
    .deinit         = replace_only_deinit,
    .load_adapter   = replace_only_load_adapter,
    .active_adapter = replace_only_active_adapter,
};

static hu_provider_t make_replace_only_provider(hu_allocator_t *alloc) {
    replace_only_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(*c));
    memset(c, 0, sizeof(*c));
    hu_provider_t prov = {.ctx = c, .vtable = &kReplaceOnlyVtable};
    return prov;
}

/* ──────────────────────────────────────────────────────────────────
 * Fixtures
 * ────────────────────────────────────────────────────────────────── */

/* Build a persona with a single overlay carrying a channel-expert path.
 * String fields are static so the persona can be stack-allocated and
 * freed by `memset(0)` in tests — we never go through `hu_persona_deinit`. */
static void fill_persona_with_overlay(hu_persona_t *persona,
                                      hu_persona_overlay_t *overlays_storage,
                                      const char *channel,
                                      const char *lora_path,
                                      const char *lora_id) {
    memset(persona, 0, sizeof(*persona));
    memset(&overlays_storage[0], 0, sizeof(overlays_storage[0]));
    overlays_storage[0].channel = (char *)channel;
    if (lora_path)
        overlays_storage[0].lora_adapter_path = (char *)lora_path;
    if (lora_id)
        overlays_storage[0].lora_adapter_id = (char *)lora_id;
    persona->overlays = overlays_storage;
    persona->overlays_count = 1;
}

/* ──────────────────────────────────────────────────────────────────
 * Tests
 * ────────────────────────────────────────────────────────────────── */

static void test_dispatcher_rejects_null_provider(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_molora_dispatcher_apply(NULL, &alloc, NULL, NULL, 0, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_dispatcher_rejects_null_alloc(void) {
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_molora_dispatcher_apply(&prov, NULL, NULL, NULL, 0, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_dispatcher_no_base_no_persona_is_noop(void) {
    /* Caller passes nothing actionable: the dispatcher must succeed
     * silently. This is the daemon's "personalization disabled" path. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create(&alloc, "mlx_qwen3", 9, NULL, 0, NULL, 0, &prov), HU_OK);
    hu_molora_apply_result_t res = {0};
    HU_ASSERT_EQ(hu_molora_dispatcher_apply(&prov, &alloc, NULL, NULL, 0, NULL, &res),
                 HU_OK);
    HU_ASSERT_FALSE(res.base_loaded);
    HU_ASSERT_FALSE(res.channel_stacked);
    HU_ASSERT_FALSE(res.channel_expert_skipped);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_dispatcher_cloud_provider_returns_not_supported(void) {
    /* The M3 cloud-fallthrough contract: openai has no `load_adapter`,
     * so REPLACE must return NOT_SUPPORTED and STACK must NOT run. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    HU_ASSERT_EQ(hu_provider_create(&alloc, "openai", 6, "test-key", 8, NULL, 0, &prov),
                 HU_OK);
    const hu_lora_adapter_spec_t base = {
        .path = "/tmp/base.lora", .path_len = 14,
        .id = "persona", .id_len = 7,
        .alloc = &alloc,
    };
    hu_persona_t persona;
    hu_persona_overlay_t overlays[1];
    fill_persona_with_overlay(&persona, overlays, "slack",
                              "/tmp/expert.lora", "slack-expert");

    hu_molora_apply_result_t res = {0};
    HU_ASSERT_EQ(hu_molora_dispatcher_apply(&prov, &alloc, &persona, "slack", 5, &base,
                                            &res),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_FALSE(res.base_loaded);
    HU_ASSERT_FALSE(res.channel_stacked);
    HU_ASSERT_FALSE(res.channel_expert_skipped);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

#ifdef HU_ENABLE_MLX_QWEN3
static void test_dispatcher_mlx_qwen3_loads_base_and_stacks_channel(void) {
    /* The happy path. mlx_qwen3 honors STACK, so an overlay-supplied
     * channel expert is layered on top of the REPLACE base. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    const hu_lora_adapter_spec_t base = {
        .path = "/tmp/base.lora", .path_len = 14,
        .id = "base", .id_len = 4,
        .alloc = &alloc,
    };
    hu_persona_t persona;
    hu_persona_overlay_t overlays[1];
    fill_persona_with_overlay(&persona, overlays, "telegram",
                              "/tmp/telegram.lora", "telegram-expert");

    hu_molora_apply_result_t res = {0};
    HU_ASSERT_EQ(hu_molora_dispatcher_apply(&prov, &alloc, &persona, "telegram", 8, &base,
                                            &res),
                 HU_OK);
    HU_ASSERT_TRUE(res.base_loaded);
    HU_ASSERT_TRUE(res.channel_stacked);
    HU_ASSERT_FALSE(res.channel_expert_skipped);
    HU_ASSERT_STR_EQ(prov.vtable->active_adapter(prov.ctx), "base");
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_dispatcher_mlx_qwen3_overlay_without_expert_skips_stack(void) {
    /* Overlay exists but has no `lora_adapter_path`; the dispatcher
     * must NOT attempt STACK. This is the common case for channels
     * that share the macro-mode base. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    const hu_lora_adapter_spec_t base = {
        .path = "/tmp/base.lora", .path_len = 14,
        .id = "base", .id_len = 4,
        .alloc = &alloc,
    };
    hu_persona_t persona;
    hu_persona_overlay_t overlays[1];
    fill_persona_with_overlay(&persona, overlays, "slack", NULL, NULL);

    hu_molora_apply_result_t res = {0};
    HU_ASSERT_EQ(hu_molora_dispatcher_apply(&prov, &alloc, &persona, "slack", 5, &base,
                                            &res),
                 HU_OK);
    HU_ASSERT_TRUE(res.base_loaded);
    HU_ASSERT_FALSE(res.channel_stacked);
    HU_ASSERT_FALSE(res.channel_expert_skipped);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_dispatcher_mlx_qwen3_unknown_channel_skips_stack(void) {
    /* The overlay covers "slack", but the dispatcher is called for
     * "imessage". `hu_persona_find_overlay` returns NULL, no STACK is
     * attempted, and the REPLACE base alone takes effect. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    const hu_lora_adapter_spec_t base = {
        .path = "/tmp/base.lora", .path_len = 14,
        .id = "base", .id_len = 4,
        .alloc = &alloc,
    };
    hu_persona_t persona;
    hu_persona_overlay_t overlays[1];
    fill_persona_with_overlay(&persona, overlays, "slack",
                              "/tmp/slack.lora", "slack");

    hu_molora_apply_result_t res = {0};
    HU_ASSERT_EQ(hu_molora_dispatcher_apply(&prov, &alloc, &persona, "imessage", 8, &base,
                                            &res),
                 HU_OK);
    HU_ASSERT_TRUE(res.base_loaded);
    HU_ASSERT_FALSE(res.channel_stacked);
    HU_ASSERT_FALSE(res.channel_expert_skipped);
    prov.vtable->deinit(prov.ctx, &alloc);
}

#if HU_IS_TEST
static void test_dispatcher_mlx_qwen3_chat_reflects_channel_expert(void) {
    /* End-to-end: after the dispatcher runs, chat() emits a mock
     * response that names both the REPLACE base and the STACKed
     * channel expert. This is the "adapter changes output" pin that
     * init-05 fidelity scoring depends on. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    const hu_lora_adapter_spec_t base = {
        .path = "/tmp/base.lora", .path_len = 14,
        .id = "seth", .id_len = 4,
        .alloc = &alloc,
    };
    hu_persona_t persona;
    hu_persona_overlay_t overlays[1];
    fill_persona_with_overlay(&persona, overlays, "imessage",
                              "/tmp/imessage.lora", "imessage-expert");

    hu_molora_apply_result_t res = {0};
    HU_ASSERT_EQ(hu_molora_dispatcher_apply(&prov, &alloc, &persona, "imessage", 8, &base,
                                            &res),
                 HU_OK);
    HU_ASSERT_TRUE(res.channel_stacked);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(prov.vtable->chat_with_system(prov.ctx, &alloc, NULL, 0, "ping", 4,
                                                "qwen3-4b", 8, 0.0, &out, &out_len),
                 HU_OK);
    HU_ASSERT_STR_CONTAINS(out, "[mlx_qwen3:seth+imessage-expert]");
    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
    prov.vtable->deinit(prov.ctx, &alloc);
}
#endif /* HU_IS_TEST */
#endif /* HU_ENABLE_MLX_QWEN3 */

static void test_dispatcher_replace_only_provider_skips_channel_expert(void) {
    /* Surrogate for huml / llamacpp: provider supports REPLACE but
     * returns NOT_SUPPORTED for STACK. Dispatcher records the skip
     * and returns HU_OK — the base is still active. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = make_replace_only_provider(&alloc);

    const hu_lora_adapter_spec_t base = {
        .path = "/tmp/base.lora", .path_len = 14,
        .id = "base", .id_len = 4,
        .alloc = &alloc,
    };
    hu_persona_t persona;
    hu_persona_overlay_t overlays[1];
    fill_persona_with_overlay(&persona, overlays, "slack",
                              "/tmp/slack.lora", "slack-expert");

    hu_molora_apply_result_t res = {0};
    HU_ASSERT_EQ(hu_molora_dispatcher_apply(&prov, &alloc, &persona, "slack", 5, &base,
                                            &res),
                 HU_OK);
    HU_ASSERT_TRUE(res.base_loaded);
    HU_ASSERT_FALSE(res.channel_stacked);
    HU_ASSERT_TRUE(res.channel_expert_skipped);
    HU_ASSERT_EQ(res.last_status, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_STR_EQ(prov.vtable->active_adapter(prov.ctx), "base");
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_dispatcher_overlay_id_falls_back_to_channel_name(void) {
    /* When `lora_adapter_id` is NULL but `lora_adapter_path` is set,
     * the dispatcher uses the channel name as the adapter id so the
     * provider's `active_adapter` accessor still reports a meaningful
     * label. We exercise via the replace-only fake to keep the
     * assertion build-flag-agnostic. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = make_replace_only_provider(&alloc);

    const hu_lora_adapter_spec_t base = {
        .path = "/tmp/base.lora", .path_len = 14,
        .id = "base", .id_len = 4,
        .alloc = &alloc,
    };
    hu_persona_t persona;
    hu_persona_overlay_t overlays[1];
    fill_persona_with_overlay(&persona, overlays, "discord",
                              "/tmp/discord.lora", NULL);

    hu_molora_apply_result_t res = {0};
    HU_ASSERT_EQ(hu_molora_dispatcher_apply(&prov, &alloc, &persona, "discord", 7, &base,
                                            &res),
                 HU_OK);
    /* STACK still got NOT_SUPPORTED — but `channel_expert_skipped`
     * being true proves the dispatcher attempted to stack rather than
     * silently skipping (which would happen if the empty-id fallback
     * was missing and the dispatcher refused to construct the spec). */
    HU_ASSERT_TRUE(res.channel_expert_skipped);
    prov.vtable->deinit(prov.ctx, &alloc);
}

void run_molora_dispatcher_tests(void) {
    HU_TEST_SUITE("MoLoRA dispatcher");
    HU_RUN_TEST(test_dispatcher_rejects_null_provider);
    HU_RUN_TEST(test_dispatcher_rejects_null_alloc);
    HU_RUN_TEST(test_dispatcher_no_base_no_persona_is_noop);
    HU_RUN_TEST(test_dispatcher_cloud_provider_returns_not_supported);
#ifdef HU_ENABLE_MLX_QWEN3
    HU_RUN_TEST(test_dispatcher_mlx_qwen3_loads_base_and_stacks_channel);
    HU_RUN_TEST(test_dispatcher_mlx_qwen3_overlay_without_expert_skips_stack);
    HU_RUN_TEST(test_dispatcher_mlx_qwen3_unknown_channel_skips_stack);
#if HU_IS_TEST
    HU_RUN_TEST(test_dispatcher_mlx_qwen3_chat_reflects_channel_expert);
#endif
#endif
    HU_RUN_TEST(test_dispatcher_replace_only_provider_skips_channel_expert);
    HU_RUN_TEST(test_dispatcher_overlay_id_falls_back_to_channel_name);
}
