/*
 * Init-04 (M3 Bridge B) — MLX Qwen3 provider tests.
 *
 * Covers the S1 deliverable from `docs/plans/2026-05-11-init-04-mlx-qwen3-provider.md`:
 *
 *   - Factory ownership (provider.ctx non-NULL, vtable wired).
 *   - NULL-argument rejection (HU_ERR_INVALID_ARGUMENT).
 *   - Identity hooks (get_name == "mlx_qwen3", supports_native_tools == false).
 *   - active_adapter starts NULL.
 *   - LoRA REPLACE state machine: load → active_adapter → unload → NULL.
 *   - Adapter id swap is atomic (new replaces old, old's bytes free'd).
 *   - Path-traversal guard ("..") rejected.
 *   - Chat round-trip in HU_IS_TEST mode (deterministic mock, no Metal).
 *   - Chat response references the active adapter id (so init-02 / #05
 *     can pin adapter-changes-output behavior on the mock when the real
 *     helper isn't available).
 *   - factory(name="mlx_qwen3") creates the provider without crashing.
 *   - Daemon-pattern: load_adapter does NOT make the provider unusable.
 *
 * Cross-initiative regression: this file does NOT modify
 * `test_provider_all.c::test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`.
 * Pin that test; we add new tests here, never edit the existing one.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#include "human/providers/mlx_qwen3.h"
#include "test_framework.h"

#include <string.h>

/* ──────────────────────────────────────────────────────────────────
 * Factory + identity
 * ────────────────────────────────────────────────────────────────── */

static void test_mlx_qwen3_create_rejects_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(NULL, &cfg, &prov), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, NULL, &prov), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_mlx_qwen3_factory_owns_ctx_and_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    HU_ASSERT_NOT_NULL(prov.vtable);
    HU_ASSERT_NOT_NULL(prov.vtable->get_name);
    HU_ASSERT_NOT_NULL(prov.vtable->chat_with_system);
    HU_ASSERT_NOT_NULL(prov.vtable->chat);
    HU_ASSERT_NOT_NULL(prov.vtable->deinit);
    /* W13 triple — present even in stub mode so the daemon's
     * personalization path can dispatch without an extra NULL check. */
    HU_ASSERT_NOT_NULL(prov.vtable->load_adapter);
    HU_ASSERT_NOT_NULL(prov.vtable->unload_adapter);
    HU_ASSERT_NOT_NULL(prov.vtable->active_adapter);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_get_name_is_canonical(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "mlx_qwen3");
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_supports_native_tools_is_false(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);
    HU_ASSERT_FALSE(prov.vtable->supports_native_tools(prov.ctx));
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_supports_streaming_is_false(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);
    /* supports_streaming is optional; the vtable wires it to a stub
     * that returns false. The dispatcher must agree. */
    HU_ASSERT_NOT_NULL(prov.vtable->supports_streaming);
    HU_ASSERT_FALSE(prov.vtable->supports_streaming(prov.ctx));
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_helper_protocol_version_is_one(void) {
    /* Pin to v1; bumping it requires a coordinated update across the
     * Python helper (scripts/mlx_qwen3_serve.py) and the protocol
     * docs. The bump itself becomes a separate test edit. */
    HU_ASSERT_EQ(hu_mlx_qwen3_helper_protocol_version(), 1);
}

/* ──────────────────────────────────────────────────────────────────
 * Adapter state machine
 * ────────────────────────────────────────────────────────────────── */

static void test_mlx_qwen3_active_adapter_starts_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);
    HU_ASSERT_NULL((void *)prov.vtable->active_adapter(prov.ctx));
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_load_adapter_rejects_invalid_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);
    const hu_lora_adapter_spec_t good = {
        .path = "/p", .path_len = 2,
        .id = "id", .id_len = 2,
        .alloc = &alloc,
    };
    /* NULL ctx */
    HU_ASSERT_EQ(prov.vtable->load_adapter(NULL, &good, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    /* NULL spec */
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, NULL, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    /* NULL path (no source) */
    hu_lora_adapter_spec_t no_path = good;
    no_path.path = NULL;
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &no_path, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    /* Empty path_len */
    hu_lora_adapter_spec_t empty_path = good;
    empty_path.path_len = 0;
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &empty_path, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    /* NULL id */
    hu_lora_adapter_spec_t no_id = good;
    no_id.id = NULL;
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &no_id, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    /* Empty id_len */
    hu_lora_adapter_spec_t empty_id = good;
    empty_id.id_len = 0;
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &empty_id, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    /* NULL alloc (provider requires it) */
    hu_lora_adapter_spec_t no_alloc = good;
    no_alloc.alloc = NULL;
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &no_alloc, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    /* STACK mode unsupported by mlx_qwen3 (MoLoRA arrives via init-02) */
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &good, HU_LORA_APPLY_MODE_STACK),
                 HU_ERR_NOT_SUPPORTED);
    prov.vtable->deinit(prov.ctx, &alloc);
}

#ifdef HU_ENABLE_MLX_QWEN3
/* Adapter state machine is only exercised when the build option is on;
 * with the option off everything (correctly) returns NOT_SUPPORTED so
 * the daemon falls through to base chat. The OFF case is tested
 * separately below. */

static void test_mlx_qwen3_load_adapter_then_active_adapter_returns_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    const char *path = "/tmp/mlx-test-adapter";
    const char *id = "persona-test";
    const hu_lora_adapter_spec_t spec = {
        .path = path, .path_len = strlen(path),
        .id = id, .id_len = strlen(id),
        .alloc = &alloc,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &spec, HU_LORA_APPLY_MODE_REPLACE),
                 HU_OK);
    const char *active = prov.vtable->active_adapter(prov.ctx);
    HU_ASSERT_NOT_NULL(active);
    HU_ASSERT_STR_EQ(active, "persona-test");
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_unload_clears_active_adapter(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    const hu_lora_adapter_spec_t spec = {
        .path = "/tmp/x", .path_len = 6,
        .id = "abc", .id_len = 3,
        .alloc = &alloc,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &spec, HU_LORA_APPLY_MODE_REPLACE), HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->active_adapter(prov.ctx), "abc");
    HU_ASSERT_EQ(prov.vtable->unload_adapter(prov.ctx, "abc", 3), HU_OK);
    HU_ASSERT_NULL((void *)prov.vtable->active_adapter(prov.ctx));
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_unload_with_nonmatching_id_is_noop(void) {
    /* The huml provider mirror — unload must be safe if a different
     * adapter is now active. Prevents races between rapid swaps and
     * lazy daemon-side unloads. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    const hu_lora_adapter_spec_t spec = {
        .path = "/tmp/a", .path_len = 6,
        .id = "first", .id_len = 5,
        .alloc = &alloc,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &spec, HU_LORA_APPLY_MODE_REPLACE), HU_OK);
    HU_ASSERT_EQ(prov.vtable->unload_adapter(prov.ctx, "second", 6), HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->active_adapter(prov.ctx), "first");
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_load_adapter_replaces_incumbent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    const hu_lora_adapter_spec_t first_spec = {
        .path = "/tmp/a", .path_len = 6,
        .id = "first", .id_len = 5,
        .alloc = &alloc,
    };
    const hu_lora_adapter_spec_t second_spec = {
        .path = "/tmp/b", .path_len = 6,
        .id = "second", .id_len = 6,
        .alloc = &alloc,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &first_spec, HU_LORA_APPLY_MODE_REPLACE),
                 HU_OK);
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &second_spec, HU_LORA_APPLY_MODE_REPLACE),
                 HU_OK);
    /* REPLACE semantics: only the most recent adapter is reported. */
    HU_ASSERT_STR_EQ(prov.vtable->active_adapter(prov.ctx), "second");
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_load_adapter_rejects_path_traversal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    /* Design doc §14 — adapter paths containing `..` are rejected
     * to defeat traversal attempts before the helper sees them. */
    const char *bad = "../etc/passwd";
    const hu_lora_adapter_spec_t bad_spec = {
        .path = bad, .path_len = strlen(bad),
        .id = "id", .id_len = 2,
        .alloc = &alloc,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &bad_spec, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL((void *)prov.vtable->active_adapter(prov.ctx));
    prov.vtable->deinit(prov.ctx, &alloc);
}

#else /* !HU_ENABLE_MLX_QWEN3 — option OFF tests */

static void test_mlx_qwen3_load_adapter_returns_not_supported_when_option_off(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);
    const hu_lora_adapter_spec_t off_spec = {
        .path = "/tmp/x", .path_len = 6,
        .id = "id", .id_len = 2,
        .alloc = &alloc,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &off_spec, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL((void *)prov.vtable->active_adapter(prov.ctx));
    /* Critical: after a NOT_SUPPORTED return, the provider must remain
     * usable. The daemon relies on this to fall through to base chat. */
    HU_ASSERT_NOT_NULL(prov.vtable);
    prov.vtable->deinit(prov.ctx, &alloc);
}

#endif /* HU_ENABLE_MLX_QWEN3 */

/* ──────────────────────────────────────────────────────────────────
 * Chat round-trip (HU_IS_TEST deterministic mock)
 * ────────────────────────────────────────────────────────────────── */

#if HU_IS_TEST && defined(HU_ENABLE_MLX_QWEN3)
static void test_mlx_qwen3_chat_returns_mock_response(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = prov.vtable->chat_with_system(prov.ctx, &alloc, "sys", 3, "hi", 2,
                                                    "qwen3-4b", 8, 0.7, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_GT(out_len, (size_t)0);
    /* No adapter loaded → response references "base". */
    HU_ASSERT_STR_CONTAINS(out, "[mlx_qwen3:base]");
    HU_ASSERT_STR_CONTAINS(out, "hi");
    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_chat_references_active_adapter_id(void) {
    /* Pins the "adapter changes output" contract used by init-02 /
     * init-05 / Track D fidelity scoring. Without this, the mock
     * response is indistinguishable across adapters and the
     * fidelity-A/B harness has nothing to compare. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    const hu_lora_adapter_spec_t seth_spec = {
        .path = "/tmp/persona-seth", .path_len = 17,
        .id = "seth", .id_len = 4,
        .alloc = &alloc,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &seth_spec, HU_LORA_APPLY_MODE_REPLACE),
                 HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = prov.vtable->chat_with_system(prov.ctx, &alloc, NULL, 0, "ping", 4,
                                                    "qwen3-4b", 8, 0.0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_CONTAINS(out, "[mlx_qwen3:seth]");
    HU_ASSERT_STR_CONTAINS(out, "ping");
    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_chat_rejects_empty_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_qwen3_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_mlx_qwen3_provider_create(&alloc, &cfg, &prov), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = prov.vtable->chat_with_system(prov.ctx, &alloc, "sys", 3, "", 0,
                                                    "qwen3-4b", 8, 0.7, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(out);
    prov.vtable->deinit(prov.ctx, &alloc);
}

#endif /* HU_IS_TEST && HU_ENABLE_MLX_QWEN3 */

/* ──────────────────────────────────────────────────────────────────
 * Factory key + daemon-pattern integration
 * ────────────────────────────────────────────────────────────────── */

static void test_mlx_qwen3_factory_key_creates_provider(void) {
    /* The factory must succeed even when the option is OFF — mirrors
     * llamacpp's "factory ok, runtime falls through" contract. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create(&alloc, "mlx_qwen3", 9, NULL, 0, NULL, 0, &prov), HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "mlx_qwen3");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_mlx_qwen3_dispatcher_load_adapter_via_factory(void) {
    /* The W13 dispatcher (hu_provider_load_adapter) is the call site
     * the daemon uses. Pin it against this provider so a future
     * helpers.c refactor that loses the NULL check on the dispatcher
     * doesn't silently break the on-device path.
     *
     * When the option is OFF the dispatcher returns NOT_SUPPORTED
     * (and the daemon falls through). When the option is ON, the
     * dispatcher returns HU_OK and active_adapter reflects the id. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create(&alloc, "mlx_qwen3", 9, NULL, 0, NULL, 0, &prov), HU_OK);
    const char *path = "/tmp/adapter";
    const char *id = "test";
    const hu_lora_adapter_spec_t spec = {
        .path = path, .path_len = strlen(path),
        .id = id, .id_len = strlen(id),
        .alloc = &alloc,
    };
    hu_error_t err = hu_provider_load_adapter(&prov, &spec, HU_LORA_APPLY_MODE_REPLACE);
#ifdef HU_ENABLE_MLX_QWEN3
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(hu_provider_active_adapter(&prov), "test");
#else
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL((void *)hu_provider_active_adapter(&prov));
#endif
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ──────────────────────────────────────────────────────────────────
 * Suite entry point
 * ────────────────────────────────────────────────────────────────── */

void run_mlx_qwen3_provider_tests(void) {
    HU_TEST_SUITE("mlx_qwen3 provider");
    HU_RUN_TEST(test_mlx_qwen3_create_rejects_null_args);
    HU_RUN_TEST(test_mlx_qwen3_factory_owns_ctx_and_vtable);
    HU_RUN_TEST(test_mlx_qwen3_get_name_is_canonical);
    HU_RUN_TEST(test_mlx_qwen3_supports_native_tools_is_false);
    HU_RUN_TEST(test_mlx_qwen3_supports_streaming_is_false);
    HU_RUN_TEST(test_mlx_qwen3_helper_protocol_version_is_one);
    HU_RUN_TEST(test_mlx_qwen3_active_adapter_starts_null);
    HU_RUN_TEST(test_mlx_qwen3_load_adapter_rejects_invalid_args);
#ifdef HU_ENABLE_MLX_QWEN3
    HU_RUN_TEST(test_mlx_qwen3_load_adapter_then_active_adapter_returns_id);
    HU_RUN_TEST(test_mlx_qwen3_unload_clears_active_adapter);
    HU_RUN_TEST(test_mlx_qwen3_unload_with_nonmatching_id_is_noop);
    HU_RUN_TEST(test_mlx_qwen3_load_adapter_replaces_incumbent);
    HU_RUN_TEST(test_mlx_qwen3_load_adapter_rejects_path_traversal);
#if HU_IS_TEST
    HU_RUN_TEST(test_mlx_qwen3_chat_returns_mock_response);
    HU_RUN_TEST(test_mlx_qwen3_chat_references_active_adapter_id);
    HU_RUN_TEST(test_mlx_qwen3_chat_rejects_empty_message);
#endif
#else
    HU_RUN_TEST(test_mlx_qwen3_load_adapter_returns_not_supported_when_option_off);
#endif
    HU_RUN_TEST(test_mlx_qwen3_factory_key_creates_provider);
    HU_RUN_TEST(test_mlx_qwen3_dispatcher_load_adapter_via_factory);
}
