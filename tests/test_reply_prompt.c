/* Offline reply-prompt rendering (src/agent/reply_prompt.c): renders the
 * llm_decides system prompt for (persona, channel, contact, incoming) with the
 * production builders, so an on-policy generator samples the real policy. */
#include "human/agent/reply_prompt.h"
#include "human/core/allocator.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static const char k_persona_json[] =
    "{\"version\":1,\"name\":\"rptest\","
    "\"core\":{\"identity\":\"Seth, a dad in St. Petersburg\",\"traits\":[\"warm\"]},"
    "\"contacts\":{\"+15550001111\":{\"name\":\"Lexi\",\"relationship\":\"romantic interest\","
    "\"dynamic\":\"Affectionate and playful. Never answer her with a lone Yeah.\","
    "\"reply_chars_p90\":50}}}";

static hu_persona_t load(hu_allocator_t *a) {
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_json(a, k_persona_json, strlen(k_persona_json), &p), HU_OK);
    return p;
}

static void reply_prompt_max_chars_uses_measured_floor(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_persona_t p = load(&a);
    hu_reply_prompt_request_t req = {.persona = &p,
                                     .channel = "imessage",
                                     .contact = "+15550001111",
                                     .incoming = "Heyo",
                                     .incoming_len = 4,
                                     .stage = HU_REL_NEW,
                                     .channel_max_chars = 200};
    /* The 2026-09-26 incident: "Heyo" capped replies at 15 chars. */
    HU_ASSERT_EQ(hu_reply_prompt_max_chars(&req), 50u);
    req.contact = "+15559999999"; /* unknown contact: no floor */
    HU_ASSERT_EQ(hu_reply_prompt_max_chars(&req), 15u);
    hu_persona_deinit(&a, &p);
}

static void reply_prompt_channel_cap_bounds_the_limit(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_persona_t p = load(&a);
    char long_msg[400];
    memset(long_msg, 'x', sizeof(long_msg));
    hu_reply_prompt_request_t req = {.persona = &p,
                                     .channel = "imessage",
                                     .contact = NULL,
                                     .incoming = long_msg,
                                     .incoming_len = sizeof(long_msg),
                                     .stage = HU_REL_DEEP,
                                     .channel_max_chars = 200};
    HU_ASSERT_EQ(hu_reply_prompt_max_chars(&req), 200u);
    hu_persona_deinit(&a, &p);
}

static void reply_prompt_render_has_lean_head_contact_and_limit(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_persona_t p = load(&a);
    hu_reply_prompt_request_t req = {.persona = &p,
                                     .channel = "imessage",
                                     .contact = "+15550001111",
                                     .incoming = "Heyo",
                                     .incoming_len = 4,
                                     .stage = HU_REL_NEW,
                                     .channel_max_chars = 200};
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_reply_prompt_render(&a, &req, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(len, strlen(out));
    HU_ASSERT_NOT_NULL(strstr(out, "You ARE this person: Seth, a dad in St. Petersburg"));
    HU_ASSERT_NOT_NULL(strstr(out, "Never answer her with a lone Yeah."));
    HU_ASSERT_NOT_NULL(strstr(out, "RESPONSE LIMIT: Maximum 50 characters"));
    a.free(a.ctx, out, len + 1);
    hu_persona_deinit(&a, &p);
}

static void reply_prompt_render_without_contact_has_no_contact_section(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_persona_t p = load(&a);
    hu_reply_prompt_request_t req = {.persona = &p,
                                     .channel = "imessage",
                                     .contact = NULL,
                                     .incoming = "Heyo",
                                     .incoming_len = 4,
                                     .stage = HU_REL_NEW,
                                     .channel_max_chars = 200};
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_reply_prompt_render(&a, &req, &out, &len), HU_OK);
    HU_ASSERT_NULL(strstr(out, "Never answer her with a lone Yeah."));
    HU_ASSERT_NOT_NULL(strstr(out, "RESPONSE LIMIT: Maximum 15 characters"));
    a.free(a.ctx, out, len + 1);
    hu_persona_deinit(&a, &p);
}

static void reply_prompt_render_rejects_missing_inputs(void) {
    hu_allocator_t a = hu_system_allocator();
    char *out = NULL;
    size_t len = 0;
    hu_reply_prompt_request_t req = {.persona = NULL, .channel = "imessage"};
    HU_ASSERT_EQ(hu_reply_prompt_render(&a, &req, &out, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_reply_prompt_render(&a, NULL, &out, &len), HU_ERR_INVALID_ARGUMENT);
}

void run_reply_prompt_tests(void) {
    HU_TEST_SUITE("reply_prompt");
    HU_RUN_TEST(reply_prompt_max_chars_uses_measured_floor);
    HU_RUN_TEST(reply_prompt_channel_cap_bounds_the_limit);
    HU_RUN_TEST(reply_prompt_render_has_lean_head_contact_and_limit);
    HU_RUN_TEST(reply_prompt_render_without_contact_has_no_contact_section);
    HU_RUN_TEST(reply_prompt_render_rejects_missing_inputs);
}
