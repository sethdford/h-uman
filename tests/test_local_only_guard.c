/* tests/test_local_only_guard.c
 *
 * The local-only backstop (owner decision 2026-10-01): with the switch LIVE,
 * no model request (generation / embedding / transcription) may reach a host
 * that is not loopback or a unix socket. Pins:
 *   - the endpoint locality rule (by URL, never by provider name), including
 *     the look-alike hosts an attacker-shaped config could use;
 *   - which URL shapes count as model requests (channel/feed APIs never do);
 *   - the HU_LOCAL_ONLY env / config / default resolution table;
 *   - the check itself in LIVE, audit (SHADOW) and OFF;
 *   - the wiring: src/core/http.c's POST entry points refuse in LIVE before
 *     the (test-mode mock) transport is reached, and are untouched in OFF.
 * No network: the HTTP layer is the HU_IS_TEST mock.
 */

#include "human/core/allocator.h"
#include "human/core/http.h"
#include "human/core/local_only_guard.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#define VERTEX_URL                                                                               \
    "https://aiplatform.googleapis.com/v1/projects/p/locations/global/publishers/google/models/" \
    "gemini-3.1-flash-lite:generateContent"
#define LOCAL_URL "http://127.0.0.1:8741/v1/chat/completions"

static bool is_local(const char *u) {
    return hu_provider_endpoint_is_local(u, u ? strlen(u) : 0);
}

/* Every test leaves the process OFF with no env override. */
static void lo_clean(void) {
    unsetenv("HU_LOCAL_ONLY");
    hu_local_only_reset();
    (void)hu_local_only_set_caller(NULL);
}

static void endpoint_loopback_and_unix_socket_are_local(void) {
    HU_ASSERT_TRUE(is_local("http://127.0.0.1:8741/v1"));
    HU_ASSERT_TRUE(is_local("http://localhost:11434"));
    HU_ASSERT_TRUE(is_local("https://LOCALHOST/v1"));
    HU_ASSERT_TRUE(is_local("http://api.localhost:9000/v1"));
    HU_ASSERT_TRUE(is_local("http://127.8.9.10/v1"));
    HU_ASSERT_TRUE(is_local("http://[::1]:8741/v1"));
    HU_ASSERT_TRUE(is_local("ws://127.0.0.1:8743/ws"));
    HU_ASSERT_TRUE(is_local("unix:/tmp/mlx.sock"));
    HU_ASSERT_TRUE(is_local("http+unix://%2Ftmp%2Fmlx.sock/v1"));
    HU_ASSERT_TRUE(is_local("/Users/me/models/glm.gguf")); /* in-process model path */
    HU_ASSERT_TRUE(is_local("http://user:pw@127.0.0.1:8741/v1"));
}

static void endpoint_cloud_and_lookalikes_are_not_local(void) {
    HU_ASSERT_FALSE(is_local(NULL));
    HU_ASSERT_FALSE(is_local(""));
    HU_ASSERT_FALSE(is_local("https://aiplatform.googleapis.com/v1/projects/p"));
    HU_ASSERT_FALSE(is_local("https://api.openai.com/v1"));
    HU_ASSERT_FALSE(is_local("http://127.0.0.1.evil.example/v1"));
    HU_ASSERT_FALSE(is_local("http://localhost.evil.example/v1"));
    HU_ASSERT_FALSE(is_local("http://localhost@evil.example/v1"));
    HU_ASSERT_FALSE(is_local("http://evil.example/?next=http://localhost"));
    HU_ASSERT_FALSE(is_local("http://127.0.0/v1"));      /* not four octets */
    HU_ASSERT_FALSE(is_local("http://127.0.0.256/v1"));  /* octet out of range */
    HU_ASSERT_FALSE(is_local("http://192.168.1.10/v1")); /* LAN is not this machine */
    HU_ASSERT_FALSE(is_local("mlx_local"));              /* a provider NAME is not an endpoint */
    /* Length-bounded: a local prefix of a longer cloud string is judged on the
     * bytes given, so the caller's length is honoured. */
    const char *u = "http://127.0.0.1:8741/v1";
    HU_ASSERT_FALSE(hu_provider_endpoint_is_local(u, 0));
}

static void model_request_shapes_match_and_channel_apis_do_not(void) {
    HU_ASSERT_TRUE(hu_local_only_url_is_model_request(VERTEX_URL));
    HU_ASSERT_TRUE(hu_local_only_url_is_model_request(
        "https://generativelanguage.googleapis.com/v1beta/models/m:streamGenerateContent?alt=sse"));
    HU_ASSERT_TRUE(hu_local_only_url_is_model_request(
        "https://aiplatform.googleapis.com/v1/projects/p/models/text-embedding-005:predict"));
    HU_ASSERT_TRUE(
        hu_local_only_url_is_model_request("https://api.openai.com/v1/chat/completions"));
    HU_ASSERT_TRUE(hu_local_only_url_is_model_request("https://api.openai.com/v1/embeddings"));
    HU_ASSERT_TRUE(
        hu_local_only_url_is_model_request("https://api.openai.com/v1/audio/transcriptions"));
    HU_ASSERT_TRUE(hu_local_only_url_is_model_request("https://api.anthropic.com/v1/messages"));
    HU_ASSERT_TRUE(hu_local_only_url_is_model_request("http://10.0.0.5:11434/api/chat"));
    /* Not model requests: auth, channels, feeds. */
    HU_ASSERT_FALSE(hu_local_only_url_is_model_request("https://oauth2.googleapis.com/token"));
    HU_ASSERT_FALSE(
        hu_local_only_url_is_model_request("https://discord.com/api/v10/channels/123/messages"));
    HU_ASSERT_FALSE(
        hu_local_only_url_is_model_request("https://api.telegram.org/botX/sendMessage"));
    HU_ASSERT_FALSE(hu_local_only_url_is_model_request("https://hnrss.org/frontpage"));
    HU_ASSERT_FALSE(hu_local_only_url_is_model_request(NULL));
}

static void env_parse_vocabulary(void) {
    HU_ASSERT_EQ(hu_local_only_env_parse("0"), 0);
    HU_ASSERT_EQ(hu_local_only_env_parse("off"), 0);
    HU_ASSERT_EQ(hu_local_only_env_parse("FALSE"), 0);
    HU_ASSERT_EQ(hu_local_only_env_parse("audit"), 1);
    HU_ASSERT_EQ(hu_local_only_env_parse("shadow"), 1);
    HU_ASSERT_EQ(hu_local_only_env_parse("1"), 2);
    HU_ASSERT_EQ(hu_local_only_env_parse("on"), 2);
    HU_ASSERT_EQ(hu_local_only_env_parse("Live"), 2);
    HU_ASSERT_EQ(hu_local_only_env_parse("true"), 2);
    HU_ASSERT_EQ(hu_local_only_env_parse(NULL), -1);
    HU_ASSERT_EQ(hu_local_only_env_parse(""), -1);
    HU_ASSERT_EQ(hu_local_only_env_parse("maybe"), -1);
}

static void resolve_table(void) {
    /* Key absent: ON exactly when the primary endpoint is local. */
    HU_ASSERT_EQ((int)hu_local_only_resolve(NULL, -1, true), (int)HU_GATE_LIVE);
    HU_ASSERT_EQ((int)hu_local_only_resolve(NULL, -1, false), (int)HU_GATE_OFF);
    /* Explicit config wins over the default either way. */
    HU_ASSERT_EQ((int)hu_local_only_resolve(NULL, 0, true), (int)HU_GATE_OFF);
    HU_ASSERT_EQ((int)hu_local_only_resolve(NULL, 1, false), (int)HU_GATE_LIVE);
    /* Env wins over config. */
    HU_ASSERT_EQ((int)hu_local_only_resolve("0", 1, true), (int)HU_GATE_OFF);
    HU_ASSERT_EQ((int)hu_local_only_resolve("1", 0, false), (int)HU_GATE_LIVE);
    HU_ASSERT_EQ((int)hu_local_only_resolve("audit", 1, true), (int)HU_GATE_SHADOW);
    /* An unrecognized env value is ignored, not treated as OFF. */
    HU_ASSERT_EQ((int)hu_local_only_resolve("maybe", 1, false), (int)HU_GATE_LIVE);
}

static void unconfigured_process_is_off(void) {
    lo_clean();
    HU_ASSERT_EQ((int)hu_local_only_mode(), (int)HU_GATE_OFF);
    HU_ASSERT_FALSE(hu_local_only_enforced());
    HU_ASSERT_EQ((int)hu_local_only_check_request(VERTEX_URL, NULL, 0), (int)HU_OK);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 0u);
}

static void live_refuses_cloud_model_request_and_counts_it(void) {
    lo_clean();
    hu_local_only_configure(HU_GATE_LIVE);
    HU_ASSERT_TRUE(hu_local_only_enforced());
    HU_ASSERT_EQ(hu_local_only_refused_count(), 0u);
    const char *prev = hu_local_only_set_caller("director");
    HU_ASSERT_EQ((int)hu_local_only_check_request(VERTEX_URL, NULL, 0),
                 (int)HU_ERR_PERMISSION_DENIED);
    (void)hu_local_only_set_caller(prev);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 1u);
    lo_clean();
}

static void live_allows_loopback_and_non_model_requests(void) {
    lo_clean();
    hu_local_only_configure(HU_GATE_LIVE);
    const char body[] = "{\"model\":\"GLM-4.5-Air-4bit\",\"messages\":[]}";
    HU_ASSERT_EQ((int)hu_local_only_check_request(LOCAL_URL, body, sizeof(body) - 1), (int)HU_OK);
    HU_ASSERT_EQ((int)hu_local_only_check_request("https://oauth2.googleapis.com/token", NULL, 0),
                 (int)HU_OK);
    HU_ASSERT_EQ((int)hu_local_only_check_request("https://hnrss.org/frontpage", NULL, 0),
                 (int)HU_OK);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 0u);
    lo_clean();
}

static void audit_mode_counts_but_never_refuses(void) {
    lo_clean();
    hu_local_only_configure(HU_GATE_SHADOW);
    HU_ASSERT_FALSE(hu_local_only_enforced());
    HU_ASSERT_EQ(hu_local_only_audit_count(), 0u);
    HU_ASSERT_EQ((int)hu_local_only_check_request(VERTEX_URL, NULL, 0), (int)HU_OK);
    HU_ASSERT_EQ(hu_local_only_audit_count(), 1u);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 0u);
    lo_clean();
}

static void env_override_beats_configured_mode(void) {
    lo_clean();
    hu_local_only_configure(HU_GATE_LIVE);
    setenv("HU_LOCAL_ONLY", "0", 1);
    HU_ASSERT_EQ((int)hu_local_only_mode(), (int)HU_GATE_OFF);
    HU_ASSERT_EQ((int)hu_local_only_check_request(VERTEX_URL, NULL, 0), (int)HU_OK);
    setenv("HU_LOCAL_ONLY", "audit", 1);
    HU_ASSERT_EQ((int)hu_local_only_mode(), (int)HU_GATE_SHADOW);
    hu_local_only_reset();
    setenv("HU_LOCAL_ONLY", "1", 1); /* env alone turns it on in an unconfigured process */
    HU_ASSERT_TRUE(hu_local_only_enforced());
    lo_clean();
}

/* Wiring: the real HTTP entry points consult the guard before the transport.
 * In test builds the transport is a mock that always answers 200, so OK in
 * OFF and PERMISSION_DENIED in LIVE can only come from the guard. */
static void http_post_refuses_cloud_model_request_when_live(void) {
    lo_clean();
    hu_allocator_t alloc = hu_system_allocator();
    const char body[] = "{\"contents\":[]}";
    hu_http_response_t resp;

    memset(&resp, 0, sizeof(resp));
    HU_ASSERT_EQ((int)hu_http_post_json(&alloc, VERTEX_URL, NULL, body, sizeof(body) - 1, &resp),
                 (int)HU_OK); /* OFF: unchanged */
    hu_http_response_free(&alloc, &resp);

    hu_local_only_configure(HU_GATE_LIVE);
    memset(&resp, 0, sizeof(resp));
    HU_ASSERT_EQ((int)hu_http_post_json(&alloc, VERTEX_URL, NULL, body, sizeof(body) - 1, &resp),
                 (int)HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_NULL(resp.body);
    HU_ASSERT_EQ((int)hu_http_post_json_stream(&alloc, VERTEX_URL, NULL, NULL, body,
                                               sizeof(body) - 1, NULL, NULL),
                 (int)HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 2u);

    memset(&resp, 0, sizeof(resp));
    HU_ASSERT_EQ((int)hu_http_post_json(&alloc, LOCAL_URL, NULL, body, sizeof(body) - 1, &resp),
                 (int)HU_OK); /* loopback still served */
    hu_http_response_free(&alloc, &resp);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 2u);
    lo_clean();
}

void run_local_only_guard_tests(void) {
    HU_TEST_SUITE("local_only_guard");
    HU_RUN_TEST(endpoint_loopback_and_unix_socket_are_local);
    HU_RUN_TEST(endpoint_cloud_and_lookalikes_are_not_local);
    HU_RUN_TEST(model_request_shapes_match_and_channel_apis_do_not);
    HU_RUN_TEST(env_parse_vocabulary);
    HU_RUN_TEST(resolve_table);
    HU_RUN_TEST(unconfigured_process_is_off);
    HU_RUN_TEST(live_refuses_cloud_model_request_and_counts_it);
    HU_RUN_TEST(live_allows_loopback_and_non_model_requests);
    HU_RUN_TEST(audit_mode_counts_but_never_refuses);
    HU_RUN_TEST(env_override_beats_configured_mode);
    HU_RUN_TEST(http_post_refuses_cloud_model_request_when_live);
}
