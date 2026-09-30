/* tests/test_turn_recording_provider.c — unit tests for the characterization
 * harness's recording provider and time-token scrubber
 * (tests/turn_recording_provider.c). The harness is only as honest as these:
 * a provider that dropped a message or a scrubber that ate a real byte would
 * make every golden comparison vacuous. */
// @covers-none — exercises the test-only helper tests/turn_recording_provider.c
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "test_framework.h"
#include "turn_recording_provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hu_chat_response_t trpt_call(trp_t *t, const hu_chat_request_t *req, hu_error_t *err_out) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t p = trp_provider(t);
    hu_chat_response_t out;
    memset(&out, 0, sizeof(out));
    *err_out = p.vtable->chat(p.ctx, &alloc, req, "m-1", 3, 0.5, &out);
    return out;
}

static void trp_records_roles_contents_and_tools(void) {
    trp_t t;
    trp_init(&t, NULL, 0, "ok.");
    hu_chat_message_t msgs[2] = {
        {.role = HU_ROLE_SYSTEM, .content = "sys", .content_len = 3},
        {.role = HU_ROLE_USER, .content = "hi\nthere", .content_len = 8},
    };
    hu_tool_spec_t tools[1] = {{.name = "memory_list",
                                .name_len = 11,
                                .description = "d",
                                .description_len = 1,
                                .parameters_json = "{}",
                                .parameters_json_len = 2}};
    hu_chat_request_t req = {
        .messages = msgs, .messages_count = 2, .tools = tools, .tools_count = 1, .max_tokens = 77};
    hu_error_t err;
    hu_chat_response_t out = trpt_call(&t, &req, &err);
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_CONTAINS(t.log, "=== chat #1\n");
    HU_ASSERT_STR_CONTAINS(t.log, "model=m-1 temperature=0.500\n");
    HU_ASSERT_STR_CONTAINS(t.log, "max_tokens=77");
    HU_ASSERT_STR_CONTAINS(t.log, "tool[0] name=memory_list");
    HU_ASSERT_STR_CONTAINS(t.log, "msg[0] role=system");
    HU_ASSERT_STR_CONTAINS(t.log, "  content=sys\n");
    HU_ASSERT_STR_CONTAINS(t.log, "msg[1] role=user");
    HU_ASSERT_STR_CONTAINS(t.log, "  content=hi\\nthere\n");
    HU_ASSERT_STR_CONTAINS(t.log, "reply=off-script\n");
    HU_ASSERT_STR_EQ(out.content, "ok.");
    HU_ASSERT_EQ(t.calls, 1);
    hu_chat_response_free(&alloc, &out);
    trp_deinit(&t);
}

/* The turn's request messages live in a per-iteration arena that is reset
 * right after the call; the log must hold the bytes as they were AT the call. */
static void trp_log_survives_request_buffer_reuse(void) {
    trp_t t;
    trp_init(&t, NULL, 0, "ok.");
    char buf[16] = "first";
    hu_chat_message_t msgs[1] = {{.role = HU_ROLE_USER, .content = buf, .content_len = 5}};
    hu_chat_request_t req = {.messages = msgs, .messages_count = 1};
    hu_error_t err;
    hu_chat_response_t out = trpt_call(&t, &req, &err);
    hu_allocator_t alloc = hu_system_allocator();
    memcpy(buf, "XXXXX", 5);
    HU_ASSERT_STR_CONTAINS(t.log, "  content=first\n");
    HU_ASSERT_STR_NOT_CONTAINS(t.log, "XXXXX");
    hu_chat_response_free(&alloc, &out);
    trp_deinit(&t);
}

static void trp_replays_scripted_tool_calls(void) {
    static const trp_step_t script[] = {
        {.err = HU_OK,
         .content = "checking",
         .tool_calls = {{"call_1", "memory_list", "{\"q\":\"a\"}"}},
         .tool_calls_count = 1},
    };
    trp_t t;
    trp_init(&t, script, 1, NULL);
    hu_chat_message_t msgs[1] = {{.role = HU_ROLE_USER, .content = "x", .content_len = 1}};
    hu_chat_request_t req = {.messages = msgs, .messages_count = 1};
    hu_error_t err;
    hu_chat_response_t out = trpt_call(&t, &req, &err);
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out.content, "checking");
    HU_ASSERT_EQ(out.tool_calls_count, 1);
    HU_ASSERT_TRUE(out.tool_calls[0].name_len == 11 &&
                   memcmp(out.tool_calls[0].name, "memory_list", 11) == 0);
    HU_ASSERT_TRUE(out.tool_calls[0].arguments_len == 9 &&
                   memcmp(out.tool_calls[0].arguments, "{\"q\":\"a\"}", 9) == 0);
    HU_ASSERT_STR_CONTAINS(t.log, "reply=step0 err=0\n");
    hu_chat_response_free(&alloc, &out); /* ASan: every scripted string is owned by alloc */
    trp_deinit(&t);
}

static void trp_off_script_fails_without_a_fallback(void) {
    trp_t t;
    trp_init(&t, NULL, 0, NULL);
    hu_chat_request_t req = {0};
    hu_error_t err;
    hu_chat_response_t out = trpt_call(&t, &req, &err);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(out.content);
    HU_ASSERT_EQ(t.calls, 1);
    trp_deinit(&t);
}

static void trp_scripted_error_is_returned_with_an_empty_response(void) {
    static const trp_step_t script[] = {{.err = HU_ERR_IO}};
    trp_t t;
    trp_init(&t, script, 1, "ok.");
    hu_chat_request_t req = {0};
    hu_error_t err;
    hu_chat_response_t out = trpt_call(&t, &req, &err);
    HU_ASSERT_EQ(err, HU_ERR_IO);
    HU_ASSERT_NULL(out.content);
    HU_ASSERT_EQ(out.tool_calls_count, 0);
    HU_ASSERT_STR_CONTAINS(t.log, "reply=step0 err=");
    trp_deinit(&t);
}

static void trp_chat_with_system_is_recorded(void) {
    trp_t t;
    trp_init(&t, NULL, 0, "ok.");
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t p = trp_provider(&t);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(p.vtable->chat_with_system(p.ctx, &alloc, "be brief", 8, "hello", 5, "m", 1, 0.2,
                                            &out, &out_len),
                 HU_OK);
    HU_ASSERT_STR_EQ(out, "ok");
    HU_ASSERT_STR_CONTAINS(t.log, "=== chat_with_system #1\n");
    HU_ASSERT_STR_CONTAINS(t.log, "system=be brief\n");
    HU_ASSERT_STR_CONTAINS(t.log, "message=hello\n");
    alloc.free(alloc.ctx, out, out_len + 1);
    trp_deinit(&t);
}

static void trp_scrub_masks_time_shaped_tokens(void) {
    const char *in = "at 2026-09-30T14:05:00Z on Tuesday, September 30 at 2:05 pm (1790000000) "
                     "9/30/2026 23rd evening x\\nMonday";
    size_t n = 0;
    char *s = trp_scrub(in, strlen(in), &n);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "at <DATE> on <DOW>, <MON> <DOM> at <TIME> (<EPOCH>) <DATE> <DOM> <TOD> "
                        "x\\n<DOW>");
    HU_ASSERT_EQ(n, strlen(s));
    free(s);
}

/* The scrubber must not eat bytes that are not time: a token budget, a slip
 * number, a tool name, a version string all survive unchanged. */
static void trp_scrub_keeps_non_time_text(void) {
    const char *in = "max_tokens=2048 slip 14 memory_list v1.2 call_1 {\"q\":\"a\"}";
    size_t n = 0;
    char *s = trp_scrub(in, strlen(in), &n);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, in);
    free(s);
}

/* The per-contact "[Temporal context]" line (src/context/contact_style_overlay.c)
 * picks one of 5 literal sentences by local hour bucket — a real TZ leak, not
 * a date/time shape. All 5 must scrub to the same fixed token so a UTC vs
 * UTC+14 characterization run compares equal regardless of local hour. */
static void trp_scrub_masks_temporal_mood_sentences(void) {
    static const char *const moods[] = {
        "It's very late/early — you shouldn't be up. Brief and sleepy.",
        "It's early morning — you're probably just waking up. Terse and groggy.",
        "It's during work hours — you might be busy. Keep it professional-ish.",
        "It's evening — you're relaxed, more chatty.",
        "It's late night — you're winding down. Reflective.",
    };
    for (size_t i = 0; i < sizeof(moods) / sizeof(moods[0]); i++) {
        char in[160];
        (void)snprintf(in, sizeof(in), "[Temporal context] %s\n", moods[i]);
        size_t n = 0;
        char *s = trp_scrub(in, strlen(in), &n);
        HU_ASSERT_NOT_NULL(s);
        HU_ASSERT_STR_EQ(s, "[Temporal context] <TEMPORAL_MOOD>\n");
        HU_ASSERT_EQ(n, strlen(s));
        free(s);
    }
}

/* src/agent/commitment.c generate_commitment_id() stamps
 * "commit-<time(NULL)>-<process-wide static counter>". The counter makes
 * the id non-repeatable across runs even with the epoch masked; scrub the
 * whole shape to one fixed token. */
static void trp_scrub_masks_commitment_ids(void) {
    const char *in = "commitment:commit-1790000000-0 and commit-1790000000-17 end";
    size_t n = 0;
    char *s = trp_scrub(in, strlen(in), &n);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "commitment:<COMMIT_ID> and <COMMIT_ID> end");
    HU_ASSERT_EQ(n, strlen(s));
    free(s);
}

void run_turn_recording_provider_tests(void) {
    HU_TEST_SUITE("TurnRecordingProvider");
    HU_RUN_TEST(trp_records_roles_contents_and_tools);
    HU_RUN_TEST(trp_log_survives_request_buffer_reuse);
    HU_RUN_TEST(trp_replays_scripted_tool_calls);
    HU_RUN_TEST(trp_off_script_fails_without_a_fallback);
    HU_RUN_TEST(trp_scripted_error_is_returned_with_an_empty_response);
    HU_RUN_TEST(trp_chat_with_system_is_recorded);
    HU_RUN_TEST(trp_scrub_masks_time_shaped_tokens);
    HU_RUN_TEST(trp_scrub_masks_temporal_mood_sentences);
    HU_RUN_TEST(trp_scrub_masks_commitment_ids);
    HU_RUN_TEST(trp_scrub_keeps_non_time_text);
}
