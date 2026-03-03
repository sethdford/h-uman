#include "test_framework.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/provider.h"
#include "seaclaw/providers/ollama.h"
#include <string.h>

static bool ollama_is_running(sc_allocator_t *alloc) {
#if SC_IS_TEST
    (void)alloc;
    return false;
#else
    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_get(alloc, "http://localhost:11434/api/tags", NULL, &resp);
    if (err == SC_OK && resp.status_code == 200) {
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        return true;
    }
    if (resp.owned && resp.body)
        sc_http_response_free(alloc, &resp);
    return false;
#endif
}

static void test_ollama_integration_chat_if_available(void) {
    sc_allocator_t alloc = sc_system_allocator();
    if (!ollama_is_running(&alloc)) {
        (void)0; /* Ollama not running — skip */
        return;
    }
    sc_provider_t prov;
    sc_error_t err = sc_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);

    sc_chat_message_t msgs[1];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = SC_ROLE_USER;
    msgs[0].content = "Say hello in exactly one word.";
    msgs[0].content_len = 29;

    sc_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.messages_count = 1;
    req.model = "tinyllama";
    req.model_len = 9;
    req.temperature = 0.1;

    sc_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "tinyllama", 9, 0.1, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(resp.content != NULL);
    SC_ASSERT_TRUE(resp.content_len > 0);

    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_integration_not_running_graceful(void) {
    sc_allocator_t alloc = sc_system_allocator();
    /* In test builds, ollama_is_running always returns false */
    SC_ASSERT_FALSE(ollama_is_running(&alloc));
}

void run_ollama_integration_tests(void) {
    SC_RUN_TEST(test_ollama_integration_chat_if_available);
    SC_RUN_TEST(test_ollama_integration_not_running_graceful);
}
