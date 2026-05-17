#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/persona.h"
#include "human/provider.h"
#include "human/provider/structured_output.h"
#include "test_framework.h"
#include <string.h>

static void schema_returns_non_null_non_empty(void) {
    const char *schema = hu_structured_output_chat_reply_schema();
    HU_ASSERT_NOT_NULL(schema);
    HU_ASSERT(hu_structured_output_chat_reply_schema_len() > 0);
    HU_ASSERT_EQ(strlen(schema), hu_structured_output_chat_reply_schema_len());
}

static void extract_reply_returns_reply_field(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char body[] = "{\"reply\":\"hello\"}";
    char *reply = NULL;
    size_t reply_len = 0;
    hu_error_t err = hu_structured_output_extract_reply(&alloc, body, sizeof(body) - 1, &reply,
                                                        &reply_len, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(reply);
    HU_ASSERT_STR_EQ(reply, "hello");
    HU_ASSERT_EQ(reply_len, (size_t)5);
    alloc.free(alloc.ctx, reply, reply_len + 1);
}

static void extract_reply_with_reasoning_preserves_both(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char body[] = "{\"reply\":\"hi there\",\"reasoning\":\"internal thought\"}";
    char *reply = NULL;
    size_t reply_len = 0;
    char *reasoning = NULL;
    size_t reasoning_len = 0;
    hu_error_t err = hu_structured_output_extract_reply(&alloc, body, sizeof(body) - 1, &reply,
                                                        &reply_len, &reasoning, &reasoning_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(reply);
    HU_ASSERT_STR_EQ(reply, "hi there");
    HU_ASSERT_NOT_NULL(reasoning);
    HU_ASSERT_STR_EQ(reasoning, "internal thought");
    alloc.free(alloc.ctx, reply, reply_len + 1);
    alloc.free(alloc.ctx, reasoning, reasoning_len + 1);
}

static void extract_reply_malformed_json_returns_parse_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char body[] = "not json at all";
    char *reply = NULL;
    size_t reply_len = 0;
    hu_error_t err = hu_structured_output_extract_reply(&alloc, body, sizeof(body) - 1, &reply,
                                                        &reply_len, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_PARSE);
    HU_ASSERT(reply == NULL);
}

static void sentinel_extract_finds_reply_markers(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char body[] = "some preamble <REPLY>foo</REPLY> trailing";
    char *reply = NULL;
    size_t reply_len = 0;
    hu_error_t err =
        hu_structured_output_extract_sentinel(&alloc, body, sizeof(body) - 1, &reply, &reply_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(reply);
    HU_ASSERT_STR_EQ(reply, "foo");
    HU_ASSERT_EQ(reply_len, (size_t)3);
    alloc.free(alloc.ctx, reply, reply_len + 1);
}

static void sentinel_extract_without_markers_returns_parse_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char body[] = "no markers here";
    char *reply = NULL;
    size_t reply_len = 0;
    hu_error_t err =
        hu_structured_output_extract_sentinel(&alloc, body, sizeof(body) - 1, &reply, &reply_len);
    HU_ASSERT_EQ(err, HU_ERR_PARSE);
    HU_ASSERT(reply == NULL);
}

/* ── Mock provider that captures the hu_chat_request_t it receives ───────── */

typedef struct {
    const char *name;
    /* captured fields from the last chat() call */
    const char *captured_response_format;
    size_t captured_response_format_len;
    const char *captured_response_schema;
    size_t captured_response_schema_len;
} req_capture_ctx_t;

static hu_error_t req_capture_chat(void *ctx, hu_allocator_t *alloc,
                                   const hu_chat_request_t *request, const char *model,
                                   size_t model_len, double temperature, hu_chat_response_t *out) {
    (void)model;
    (void)model_len;
    (void)temperature;
    req_capture_ctx_t *c = (req_capture_ctx_t *)ctx;
    c->captured_response_format = request ? request->response_format : NULL;
    c->captured_response_format_len = request ? request->response_format_len : 0;
    c->captured_response_schema = request ? request->response_schema : NULL;
    c->captured_response_schema_len = request ? request->response_schema_len : 0;
    const char *resp = "mock";
    out->content = hu_strndup(alloc, resp, strlen(resp));
    out->content_len = out->content ? strlen(resp) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->usage.prompt_tokens = 1;
    out->usage.completion_tokens = 1;
    out->usage.total_tokens = 2;
    out->model = NULL;
    out->model_len = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static hu_error_t req_capture_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                               const char *system_prompt, size_t system_prompt_len,
                                               const char *message, size_t message_len,
                                               const char *model, size_t model_len,
                                               double temperature, char **out, size_t *out_len) {
    (void)ctx;
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    const char *resp = "mock";
    *out = hu_strndup(alloc, resp, strlen(resp));
    *out_len = *out ? strlen(resp) : 0;
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static bool req_capture_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}
static const char *req_capture_get_name(void *ctx) {
    return ((req_capture_ctx_t *)ctx)->name;
}
static void req_capture_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t req_capture_vtable = {
    .chat_with_system = req_capture_chat_with_system,
    .chat = req_capture_chat,
    .supports_native_tools = req_capture_supports_native_tools,
    .get_name = req_capture_get_name,
    .deinit = req_capture_deinit,
};

/* ── Persona wiring tests (AC1–AC4) — end-to-end through agent_turn ──────── */

/* HIGH #3 (Sprint 3 critic): when persona->structured_output_enabled is true,
 * agent_turn.c must set request->response_format = "json_schema" and
 * request->response_schema to the JSON schema string.  This test uses a
 * capturing mock provider so it exercises the PRODUCTION wiring in
 * agent_turn.c rather than inlining the conditional. */
static void persona_opt_in_sets_request_fields_via_agent_turn(void) {
    hu_allocator_t alloc = hu_system_allocator();

    req_capture_ctx_t cap;
    memset(&cap, 0, sizeof(cap));
    cap.name = "mock";
    hu_provider_t prov = {.ctx = &cap, .vtable = &req_capture_vtable};

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "test-model",
                             10, "mock", 4, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    /* Attach a persona with structured_output_enabled = true. */
    hu_persona_t persona_storage;
    memset(&persona_storage, 0, sizeof(persona_storage));
    persona_storage.structured_output_enabled = true;
    agent.persona = &persona_storage;

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "hello", 5, &response, &response_len);
    HU_ASSERT_EQ(err, HU_OK);

    /* The production code in agent_turn.c must have set response_format on the
     * request it passed to the provider. */
    HU_ASSERT_NOT_NULL(cap.captured_response_format);
    HU_ASSERT_STR_EQ(cap.captured_response_format, "json_schema");
    HU_ASSERT_EQ(cap.captured_response_format_len, (size_t)11);
    HU_ASSERT_NOT_NULL(cap.captured_response_schema);
    HU_ASSERT(cap.captured_response_schema_len > 0);

    agent.persona = NULL; /* persona is stack-allocated; clear before deinit */
    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

/* When structured_output_enabled is false the provider must receive NULL
 * response_format (production code gates on the flag). */
static void persona_opt_out_leaves_request_fields_null_via_agent_turn(void) {
    hu_allocator_t alloc = hu_system_allocator();

    req_capture_ctx_t cap;
    memset(&cap, 0, sizeof(cap));
    cap.name = "mock";
    /* Pre-poison to confirm the field is written (not just default-zero). */
    cap.captured_response_format = (const char *)0xdeadbeef;
    hu_provider_t prov = {.ctx = &cap, .vtable = &req_capture_vtable};

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "test-model",
                             10, "mock", 4, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    hu_persona_t persona_storage;
    memset(&persona_storage, 0, sizeof(persona_storage));
    persona_storage.structured_output_enabled = false;
    agent.persona = &persona_storage;

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "hello", 5, &response, &response_len);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT(cap.captured_response_format == NULL);
    HU_ASSERT(cap.captured_response_schema == NULL);

    agent.persona = NULL;
    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

/* NULL persona pointer must be safe — agent_turn.c guards on agent->persona != NULL. */
static void null_persona_leaves_request_fields_null_via_agent_turn(void) {
    hu_allocator_t alloc = hu_system_allocator();

    req_capture_ctx_t cap;
    memset(&cap, 0, sizeof(cap));
    cap.name = "mock";
    cap.captured_response_format = (const char *)0xdeadbeef;
    hu_provider_t prov = {.ctx = &cap, .vtable = &req_capture_vtable};

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "test-model",
                             10, "mock", 4, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    /* agent.persona stays NULL (default from hu_agent_from_config with NULL persona arg). */
    HU_ASSERT(agent.persona == NULL);

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "hello", 5, &response, &response_len);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT(cap.captured_response_format == NULL);
    HU_ASSERT(cap.captured_response_schema == NULL);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

void run_structured_output_tests(void) {
    HU_TEST_SUITE("structured_output");
    HU_RUN_TEST(schema_returns_non_null_non_empty);
    HU_RUN_TEST(extract_reply_returns_reply_field);
    HU_RUN_TEST(extract_reply_with_reasoning_preserves_both);
    HU_RUN_TEST(extract_reply_malformed_json_returns_parse_error);
    HU_RUN_TEST(sentinel_extract_finds_reply_markers);
    HU_RUN_TEST(sentinel_extract_without_markers_returns_parse_error);
    HU_RUN_TEST(persona_opt_in_sets_request_fields_via_agent_turn);
    HU_RUN_TEST(persona_opt_out_leaves_request_fields_null_via_agent_turn);
    HU_RUN_TEST(null_persona_leaves_request_fields_null_via_agent_turn);
}
