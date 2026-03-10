#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/provider.h"
#include "human/providers/ollama.h"
#include "test_framework.h"
#include <string.h>

static bool ollama_is_running(hu_allocator_t *alloc) {
#if HU_IS_TEST
    (void)alloc;
    return false;
#else
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, "http://localhost:11434/api/tags", NULL, &resp);
    if (err == HU_OK && resp.status_code == 200) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return true;
    }
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    return false;
#endif
}

static void test_ollama_integration_chat_if_available(void) {
    hu_allocator_t alloc = hu_system_allocator();
    if (!ollama_is_running(&alloc)) {
        (void)0; /* Ollama not running — skip */
        return;
    }
    hu_provider_t prov;
    hu_error_t err = hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);

    hu_chat_message_t msgs[1];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = HU_ROLE_USER;
    msgs[0].content = "Say hello in exactly one word.";
    msgs[0].content_len = 29;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.messages_count = 1;
    req.model = "tinyllama";
    req.model_len = 9;
    req.temperature = 0.1;

    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "tinyllama", 9, 0.1, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(resp.content != NULL);
    HU_ASSERT_TRUE(resp.content_len > 0);

    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_create_null_alloc_fails(void) {
    hu_provider_t prov;
    hu_error_t err = hu_ollama_create(NULL, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_ollama_create_and_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    HU_ASSERT_NOT_NULL(prov.vtable);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "ollama");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_integration_not_running_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* In test builds, ollama_is_running always returns false */
    HU_ASSERT_FALSE(ollama_is_running(&alloc));
}

void run_ollama_integration_tests(void) {
    HU_TEST_SUITE("Ollama integration");
    HU_RUN_TEST(test_ollama_integration_chat_if_available);
    HU_RUN_TEST(test_ollama_integration_not_running_graceful);
    HU_RUN_TEST(test_ollama_create_null_alloc_fails);
    HU_RUN_TEST(test_ollama_create_and_destroy);
}
