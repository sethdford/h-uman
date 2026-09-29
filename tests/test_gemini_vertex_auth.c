/* Vertex-vs-generative-language auth classification for the Gemini provider.
 *
 * Regression pin for the 2026-07-25 incident: when local MLX was unreachable,
 * the cloud fallback built a Vertex URL (aiplatform.googleapis.com/...) but
 * appended a `?key=AIza...` API key. Vertex rejects API keys with HTTP 401, so
 * every fallback reply failed and the daemon could not answer iMessage at all.
 *
 * The real request builder lives behind `#if !HU_IS_TEST`, so these tests pin
 * the pure predicate that DRIVES the decision: `hu_gemini_base_is_vertex`.
 * A Vertex base MUST classify true (→ OAuth bearer, never `?key=`); the
 * generative-language endpoint and OpenAI-compatible hosts MUST classify false
 * (→ `?key=` is legitimate there). */

#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/provider.h"
#include "human/providers/gemini.h"

#include <string.h>

/* The exact base_url from the live fallback config that produced the 401. */
#define VERTEX_BASE                                                                                \
    "https://aiplatform.googleapis.com/v1/projects/johnb-2025/locations/global/publishers/google/" \
    "models"

static void test_vertex_host_classifies_as_vertex(void) {
    HU_ASSERT_TRUE(hu_gemini_base_is_vertex(VERTEX_BASE, strlen(VERTEX_BASE)));

    const char *bare = "https://aiplatform.googleapis.com";
    HU_ASSERT_TRUE(hu_gemini_base_is_vertex(bare, strlen(bare)));

    /* Regional host is still Vertex. */
    const char *regional = "https://us-central1-aiplatform.googleapis.com/v1/projects/p/locations/"
                           "us-central1/publishers/google/models";
    HU_ASSERT_TRUE(hu_gemini_base_is_vertex(regional, strlen(regional)));
}

static void test_generative_language_host_is_not_vertex(void) {
    /* The generative-language endpoint is the ONLY host where `?key=` is valid. */
    const char *genlang = "https://generativelanguage.googleapis.com/v1beta/models";
    HU_ASSERT_FALSE(hu_gemini_base_is_vertex(genlang, strlen(genlang)));
}

static void test_local_and_compat_hosts_are_not_vertex(void) {
    const char *mlx = "http://127.0.0.1:8741/v1";
    HU_ASSERT_FALSE(hu_gemini_base_is_vertex(mlx, strlen(mlx)));

    const char *openai = "https://api.openai.com/v1";
    HU_ASSERT_FALSE(hu_gemini_base_is_vertex(openai, strlen(openai)));
}

static void test_null_empty_and_short_inputs_are_not_vertex(void) {
    HU_ASSERT_FALSE(hu_gemini_base_is_vertex(NULL, 0));
    HU_ASSERT_FALSE(hu_gemini_base_is_vertex("", 0));
    HU_ASSERT_FALSE(hu_gemini_base_is_vertex("aiplat", 6)); /* shorter than needle */
}

static void test_length_bound_is_respected(void) {
    /* The needle sits past the supplied length — must NOT match (no over-read). */
    const char *s = "https://aiplatform.googleapis.com/models";
    HU_ASSERT_FALSE(hu_gemini_base_is_vertex(s, 8)); /* only "https://" is in scope */
}

/* Creating a Gemini provider with a Vertex base AND an api_key must succeed and
 * tear down cleanly — the create path drops the (unusable) key for a Vertex
 * endpoint rather than wiring it into a `?key=` URL. ASan pins clean teardown. */
static void test_create_vertex_base_with_api_key_is_clean(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t provider = {0};
    const char *key = "AIzaSyExampleKeyDoesNotMatterForVertex";

    HU_ASSERT_EQ(
        hu_gemini_create(&alloc, key, strlen(key), VERTEX_BASE, strlen(VERTEX_BASE), &provider),
        HU_OK);
    HU_ASSERT_NOT_NULL(provider.ctx);
    HU_ASSERT_NOT_NULL(provider.vtable);
    provider.vtable->deinit(provider.ctx, &alloc);
}

void run_gemini_vertex_auth_tests(void) {
    HU_TEST_SUITE("gemini-vertex-auth");
    HU_RUN_TEST(test_vertex_host_classifies_as_vertex);
    HU_RUN_TEST(test_generative_language_host_is_not_vertex);
    HU_RUN_TEST(test_local_and_compat_hosts_are_not_vertex);
    HU_RUN_TEST(test_null_empty_and_short_inputs_are_not_vertex);
    HU_RUN_TEST(test_length_bound_is_respected);
    HU_RUN_TEST(test_create_vertex_base_with_api_key_is_clean);
}
