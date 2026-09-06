/* Task 5b — the HTTP embedder's parser is a pure predicate over the server's
 * JSON; it must refuse every shape that would otherwise become a zero vector
 * (and score as a measurement). */
#include "human/core/allocator.h"
#include "human/memory/vector/embedder_http.h"
#include "test_framework.h"
#include <string.h>

static const char GOOD[] =
    "{\"object\":\"list\",\"model\":\"m\",\"data\":[{\"object\":\"embedding\",\"index\":0,"
    "\"embedding\":[0.5,0.5,0.0]},{\"object\":\"embedding\",\"index\":1,\"embedding\":[0.0,1.0,0.0]"
    "}],"
    "\"usage\":{\"prompt_tokens\":4,\"total_tokens\":4}}";

static void test_parse_two_vectors(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedding_t out[2];
    HU_ASSERT_EQ(hu_embedder_http_parse_response(&alloc, GOOD, strlen(GOOD), 2, out), HU_OK);
    HU_ASSERT_EQ((long)out[0].dim, 3L);
    HU_ASSERT_EQ((long)out[1].dim, 3L);
    HU_ASSERT_FLOAT_EQ(out[0].values[0], 0.5f, 1e-6f);
    HU_ASSERT_FLOAT_EQ(out[1].values[1], 1.0f, 1e-6f);
    for (int i = 0; i < 2; i++)
        alloc.free(alloc.ctx, out[i].values, out[i].dim * sizeof(float));
}

static void test_parse_refuses_count_mismatch_and_ragged_and_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedding_t out[2];
    /* expected 1, got 2 */
    HU_ASSERT_EQ(hu_embedder_http_parse_response(&alloc, GOOD, strlen(GOOD), 1, out),
                 HU_ERR_PROVIDER_RESPONSE);
    const char ragged[] = "{\"data\":[{\"embedding\":[1,2,3]},{\"embedding\":[1,2]}]}";
    HU_ASSERT_EQ(hu_embedder_http_parse_response(&alloc, ragged, strlen(ragged), 2, out),
                 HU_ERR_PROVIDER_RESPONSE);
    HU_ASSERT_NULL(out[0].values); /* nothing left allocated on failure */
    const char empty[] = "{\"data\":[{\"embedding\":[]}]}";
    HU_ASSERT_EQ(hu_embedder_http_parse_response(&alloc, empty, strlen(empty), 1, out),
                 HU_ERR_PROVIDER_RESPONSE);
    const char err[] = "{\"error\":\"embedding failed\"}";
    HU_ASSERT_EQ(hu_embedder_http_parse_response(&alloc, err, strlen(err), 1, out),
                 HU_ERR_PROVIDER_RESPONSE);
    const char junk[] = "<html>502</html>";
    HU_ASSERT_EQ(hu_embedder_http_parse_response(&alloc, junk, strlen(junk), 1, out),
                 HU_ERR_PROVIDER_RESPONSE);
}

static void test_create_builds_url_and_rejects_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedder_t e = hu_embedder_http_create(&alloc, "http://127.0.0.1:8741/");
    HU_ASSERT_NOT_NULL(e.ctx);
    HU_ASSERT_EQ((long)e.vtable->dimensions(e.ctx), 0L); /* unknown until first response */
    e.vtable->deinit(e.ctx, &alloc);
    hu_embedder_t none = hu_embedder_http_create(&alloc, "");
    HU_ASSERT_NULL(none.ctx);
}

void run_embedder_http_tests(void) {
    HU_TEST_SUITE("embedder_http");
    HU_RUN_TEST(test_parse_two_vectors);
    HU_RUN_TEST(test_parse_refuses_count_mismatch_and_ragged_and_empty);
    HU_RUN_TEST(test_create_builds_url_and_rejects_empty);
}
