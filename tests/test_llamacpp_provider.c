/*
 * llama.cpp provider scaffold tests (W13 Bridge A).
 *
 * Verify the dispatcher shape: factory always succeeds, the chat hook
 * and adapter hooks return HU_ERR_NOT_SUPPORTED until libllama is
 * linked. This isolates the integration point so a future libllama
 * vendor-in only has to flip the assertions, not write the harness.
 */

#include "human/providers/llamacpp.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <string.h>

static hu_allocator_t alloc(void) {
    return hu_system_allocator();
}

static void test_llamacpp_factory_creates_with_minimal_config(void) {
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);
    HU_ASSERT_NOT_NULL(prov.vtable);
    HU_ASSERT_NOT_NULL(prov.vtable->get_name);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "llamacpp");
    HU_ASSERT_FALSE(prov.vtable->supports_native_tools(prov.ctx));
    prov.vtable->deinit(prov.ctx, &a);
}

static void test_llamacpp_factory_owns_model_path_copy(void) {
    /* The factory must duplicate the caller's model_path so the
     * caller can free its config struct after create() returns. We
     * can't introspect the owned pointer directly, but we can prove
     * the contract by overwriting the source buffer post-create
     * and confirming the provider still deinits cleanly. */
    hu_allocator_t a = alloc();
    char path[64];
    strcpy(path, "/tmp/persona-default.gguf");
    hu_llamacpp_config_t cfg = {.model_path = path};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);
    memset(path, 0, sizeof(path));
    prov.vtable->deinit(prov.ctx, &a);
}

static void test_llamacpp_factory_rejects_null_args(void) {
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(NULL, &cfg, &prov),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, NULL, &prov),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_llamacpp_chat_returns_not_supported_until_linked(void) {
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    /* Even with valid inputs, the unlinked vtable must return a clean
     * NOT_SUPPORTED so the agent can fall back to the base provider. */
    HU_ASSERT_EQ(prov.vtable->chat_with_system(prov.ctx, &a, "sys", 3,
                                                "hello", 5, "model", 5, 0.5,
                                                &out, &out_len),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT(out == NULL);
    prov.vtable->deinit(prov.ctx, &a);
}

static void test_llamacpp_chat_multimessage_returns_not_supported(void) {
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);
    HU_ASSERT_NOT_NULL(prov.vtable->chat);
    hu_chat_message_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = "sys";
    msgs[0].content_len = 3;
    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = "hi";
    msgs[1].content_len = 2;
    hu_chat_request_t req = {.messages = msgs,
                            .messages_count = 2,
                            .model = "m",
                            .model_len = 1,
                            .temperature = 0.5};
    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    HU_ASSERT_EQ(prov.vtable->chat(prov.ctx, &a, &req, "m", 1, 0.5, &resp), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(resp.content);
    prov.vtable->deinit(prov.ctx, &a);
}

static void test_llamacpp_chat_rejects_null_args(void) {
    /* The multi-message dispatcher must be NULL-arg safe so the agent
     * loop surfaces HU_ERR_INVALID_ARGUMENT instead of segfaulting on
     * a malformed request from a misconfigured caller. */
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);
    HU_ASSERT_NOT_NULL(prov.vtable->chat);

    hu_chat_message_t msg = {.role = HU_ROLE_USER, .content = "hi", .content_len = 2};
    hu_chat_request_t req = {.messages = &msg, .messages_count = 1};
    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));

    HU_ASSERT_EQ(prov.vtable->chat(NULL, &a, &req, "m", 1, 0.5, &resp), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(prov.vtable->chat(prov.ctx, NULL, &req, "m", 1, 0.5, &resp),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(prov.vtable->chat(prov.ctx, &a, NULL, "m", 1, 0.5, &resp),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(prov.vtable->chat(prov.ctx, &a, &req, "m", 1, 0.5, NULL), HU_ERR_INVALID_ARGUMENT);

    prov.vtable->deinit(prov.ctx, &a);
}

static void test_llamacpp_supports_streaming_is_false(void) {
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);
    HU_ASSERT_NOT_NULL(prov.vtable->supports_streaming);
    HU_ASSERT_FALSE(prov.vtable->supports_streaming(prov.ctx));
    prov.vtable->deinit(prov.ctx, &a);
}

/* Phase 1 (RL SOTA) — this stub assertion is only valid when
 * llama.cpp is NOT linked. With HU_ENABLE_LLAMACPP=ON the vendored
 * library provides real load_adapter/unload_adapter implementations
 * that reach the GGUF loader and return HU_ERR_PROVIDER_RESPONSE for
 * a nonexistent path (or HU_OK + state mutation for a real one), not
 * HU_ERR_NOT_SUPPORTED. The linked-path coverage lives in Task 9's
 * tests/test_llamacpp_lora_hotswap.c. */
#if !defined(HU_ENABLE_LLAMACPP)
static void test_llamacpp_load_adapter_returns_not_supported_until_linked(void) {
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);
    /* Bypass the helper: invoke the vtable hook directly. */
    HU_ASSERT_NOT_NULL(prov.vtable->load_adapter);
    const hu_lora_adapter_spec_t spec = {
        .path = "/tmp/x.lora", .path_len = 11,
        .id = "x", .id_len = 1,
        .alloc = &a,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &spec, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NOT_NULL(prov.vtable->unload_adapter);
    HU_ASSERT_EQ(prov.vtable->unload_adapter(prov.ctx, "x", 1),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NOT_NULL(prov.vtable->active_adapter);
    HU_ASSERT(prov.vtable->active_adapter(prov.ctx) == NULL);
    prov.vtable->deinit(prov.ctx, &a);
}
#endif  /* !HU_ENABLE_LLAMACPP */

static void test_llamacpp_load_adapter_rejects_null_args(void) {
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);
    const hu_lora_adapter_spec_t good = {
        .path = "/x", .path_len = 2,
        .id = "x", .id_len = 1,
        .alloc = &a,
    };
    /* NULL ctx */
    HU_ASSERT_EQ(prov.vtable->load_adapter(NULL, &good, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    /* NULL spec */
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, NULL, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    /* No source (NULL path AND NULL bytes) */
    hu_lora_adapter_spec_t no_src = good;
    no_src.path = NULL;
    no_src.path_len = 0;
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &no_src, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    /* Empty id */
    hu_lora_adapter_spec_t empty_id = good;
    empty_id.id = NULL;
    empty_id.id_len = 0;
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &empty_id, HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);
    /* STACK mode unsupported by llamacpp (MoLoRA arrives via init-02) */
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &good, HU_LORA_APPLY_MODE_STACK),
                 HU_ERR_NOT_SUPPORTED);
    /* S1.5 security review MEDIUM-1: out-of-range mode rejected with
     * INVALID_ARGUMENT, not silently treated as REPLACE. */
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &good, (hu_lora_apply_mode_t)99),
                 HU_ERR_INVALID_ARGUMENT);
    prov.vtable->deinit(prov.ctx, &a);
}

/* S1.5 security review CRITICAL-2: ensure path traversal (`..`) and
 * embedded NUL bytes are rejected with HU_ERR_INVALID_ARGUMENT before
 * any fopen-equivalent runs. Mirrors the mlx_qwen3 reference test. */
static void test_llamacpp_load_adapter_rejects_path_traversal(void) {
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);
    HU_ASSERT_NOT_NULL(prov.vtable->load_adapter);

    /* Classic `..` traversal — must be rejected pre-fopen. */
    const char *traversal = "../etc/passwd";
    const hu_lora_adapter_spec_t bad_dotdot = {
        .path = traversal, .path_len = strlen(traversal),
        .id = "id", .id_len = 2,
        .alloc = &a,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &bad_dotdot,
                                            HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);

    /* `..` deep inside a long path — still rejected. */
    const char *deep = "/usr/share/lora/../../../etc/shadow";
    const hu_lora_adapter_spec_t bad_deep = {
        .path = deep, .path_len = strlen(deep),
        .id = "id", .id_len = 2,
        .alloc = &a,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &bad_deep,
                                            HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);

    /* Embedded NUL — defeats any downstream C-string handling. */
    const char nul_path[] = "/tmp/safe\0/etc/shadow";
    const hu_lora_adapter_spec_t bad_nul = {
        .path = nul_path, .path_len = sizeof(nul_path) - 1,
        .id = "id", .id_len = 2,
        .alloc = &a,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &bad_nul,
                                            HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);

    /* Trailing NUL (LOW-2): also rejected. */
    const char trail_nul[] = "/tmp/safe.lora\0";
    const hu_lora_adapter_spec_t bad_trail = {
        .path = trail_nul, .path_len = sizeof(trail_nul) - 1,
        .id = "id", .id_len = 2,
        .alloc = &a,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &bad_trail,
                                            HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_INVALID_ARGUMENT);

    prov.vtable->deinit(prov.ctx, &a);
}

/* S1.5 critic PE2: bytes-only spec on a path-only provider must return
 * HU_ERR_NOT_SUPPORTED, not HU_ERR_INVALID_ARGUMENT, so init-08
 * federated-LoRA capability detection can distinguish "feature missing"
 * from "API misuse". */
static void test_llamacpp_load_adapter_bytes_only_spec_returns_not_supported(void) {
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);

    const uint8_t fake_weights[] = {0xDE, 0xAD, 0xBE, 0xEF};
    const hu_lora_adapter_spec_t bytes_only = {
        .path = NULL,
        .path_len = 0,
        .bytes = fake_weights,
        .bytes_len = sizeof(fake_weights),
        .id = "id",
        .id_len = 2,
        .alloc = &a,
    };
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &bytes_only,
                                            HU_LORA_APPLY_MODE_REPLACE),
                 HU_ERR_NOT_SUPPORTED);
    prov.vtable->deinit(prov.ctx, &a);
}

void run_llamacpp_provider_tests(void) {
    HU_TEST_SUITE("Llamacpp Provider (W13 Bridge A scaffold)");
    HU_RUN_TEST(test_llamacpp_factory_creates_with_minimal_config);
    HU_RUN_TEST(test_llamacpp_factory_owns_model_path_copy);
    HU_RUN_TEST(test_llamacpp_factory_rejects_null_args);
    HU_RUN_TEST(test_llamacpp_chat_returns_not_supported_until_linked);
    HU_RUN_TEST(test_llamacpp_chat_multimessage_returns_not_supported);
    HU_RUN_TEST(test_llamacpp_chat_rejects_null_args);
    HU_RUN_TEST(test_llamacpp_supports_streaming_is_false);
#if !defined(HU_ENABLE_LLAMACPP)
    HU_RUN_TEST(test_llamacpp_load_adapter_returns_not_supported_until_linked);
#endif
    HU_RUN_TEST(test_llamacpp_load_adapter_rejects_null_args);
    HU_RUN_TEST(test_llamacpp_load_adapter_rejects_path_traversal);
    HU_RUN_TEST(test_llamacpp_load_adapter_bytes_only_spec_returns_not_supported);
}
