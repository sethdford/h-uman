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

static void test_llamacpp_load_adapter_returns_not_supported_until_linked(void) {
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);
    /* Bypass the helper: invoke the vtable hook directly. */
    HU_ASSERT_NOT_NULL(prov.vtable->load_adapter);
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &a, "/tmp/x.lora", 11,
                                            "x", 1),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NOT_NULL(prov.vtable->unload_adapter);
    HU_ASSERT_EQ(prov.vtable->unload_adapter(prov.ctx, "x", 1),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NOT_NULL(prov.vtable->active_adapter);
    HU_ASSERT(prov.vtable->active_adapter(prov.ctx) == NULL);
    prov.vtable->deinit(prov.ctx, &a);
}

static void test_llamacpp_load_adapter_rejects_null_args(void) {
    hu_allocator_t a = alloc();
    hu_llamacpp_config_t cfg = {0};
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_llamacpp_provider_create(&a, &cfg, &prov), HU_OK);
    HU_ASSERT_EQ(prov.vtable->load_adapter(NULL, &a, "/x", 2, "x", 1),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, NULL, "/x", 2, "x", 1),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &a, NULL, 0, "x", 1),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(prov.vtable->load_adapter(prov.ctx, &a, "/x", 2, NULL, 0),
                 HU_ERR_INVALID_ARGUMENT);
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
    HU_RUN_TEST(test_llamacpp_load_adapter_returns_not_supported_until_linked);
    HU_RUN_TEST(test_llamacpp_load_adapter_rejects_null_args);
}
