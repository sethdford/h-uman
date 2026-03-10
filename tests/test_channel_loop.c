#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <string.h>

static void test_channel_loop_state_init(void) {
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    HU_ASSERT_FALSE(hu_channel_loop_should_stop(&state));
}

static void test_channel_loop_request_stop(void) {
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    hu_channel_loop_request_stop(&state);
    HU_ASSERT_TRUE(hu_channel_loop_should_stop(&state));
}

static void test_channel_loop_should_stop_null_returns_true(void) {
    HU_ASSERT_TRUE(hu_channel_loop_should_stop(NULL));
}

static void test_channel_loop_touch_updates_activity(void) {
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    hu_channel_loop_touch(&state);
    HU_ASSERT(state.last_activity > 0);
}

static hu_error_t noop_poll(void *ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count) {
    (void)ctx;
    (void)alloc;
    (void)msgs;
    (void)max_msgs;
    *out_count = 0;
    return HU_OK;
}

static hu_error_t dispatch_echo(void *ctx, const char *session_key, const char *content,
                                char **response_out) {
    (void)ctx;
    (void)session_key;
    (void)content;
    *response_out = NULL;
    return HU_OK;
}

static void test_channel_loop_tick_empty_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    hu_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .channel_ctx = NULL,
        .agent_ctx = NULL,
        .poll_fn = noop_poll,
        .dispatch_fn = dispatch_echo,
        .evict_fn = NULL,
        .evict_ctx = NULL,
        .evict_interval = 0,
        .idle_timeout_secs = 0,
    };
    int processed = -1;
    hu_error_t err = hu_channel_loop_tick(&ctx, &state, &processed);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(processed, 0);
}

static void test_channel_loop_tick_null_ctx(void) {
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    int processed = 0;
    hu_error_t err = hu_channel_loop_tick(NULL, &state, &processed);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_channel_loop_tick_null_state(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = noop_poll,
        .dispatch_fn = dispatch_echo,
    };
    int processed = 0;
    hu_error_t err = hu_channel_loop_tick(&ctx, NULL, &processed);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_channel_loop_tick_null_alloc(void) {
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    hu_channel_loop_ctx_t ctx = {
        .alloc = NULL,
        .channel_ctx = NULL,
        .agent_ctx = NULL,
        .poll_fn = noop_poll,
        .dispatch_fn = dispatch_echo,
    };
    int processed = 0;
    hu_error_t err = hu_channel_loop_tick(&ctx, &state, &processed);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_channel_loop_tick_null_poll_fn(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    hu_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = NULL,
        .dispatch_fn = dispatch_echo,
    };
    int processed = 0;
    hu_error_t err = hu_channel_loop_tick(&ctx, &state, &processed);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_channel_loop_tick_null_dispatch_fn(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    hu_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = noop_poll,
        .dispatch_fn = NULL,
    };
    int processed = 0;
    hu_error_t err = hu_channel_loop_tick(&ctx, &state, &processed);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static hu_error_t poll_return_two(void *ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                                  size_t max_msgs, size_t *out_count) {
    (void)ctx;
    (void)alloc;
    if (max_msgs < 2) {
        *out_count = 0;
        return HU_OK;
    }
    strncpy(msgs[0].session_key, "sess1", sizeof(msgs[0].session_key) - 1);
    msgs[0].session_key[sizeof(msgs[0].session_key) - 1] = '\0';
    strncpy(msgs[0].content, "hello", sizeof(msgs[0].content) - 1);
    msgs[0].content[sizeof(msgs[0].content) - 1] = '\0';
    msgs[0].message_id = -1;
    strncpy(msgs[1].session_key, "sess2", sizeof(msgs[1].session_key) - 1);
    msgs[1].session_key[sizeof(msgs[1].session_key) - 1] = '\0';
    strncpy(msgs[1].content, "world", sizeof(msgs[1].content) - 1);
    msgs[1].content[sizeof(msgs[1].content) - 1] = '\0';
    msgs[1].message_id = -1;
    *out_count = 2;
    return HU_OK;
}

static int dispatch_invoked_count;
static const char *dispatch_last_session;
static const char *dispatch_last_content;
static hu_error_t dispatch_count(void *ctx, const char *session_key, const char *content,
                                 char **response_out) {
    (void)ctx;
    *response_out = NULL;
    dispatch_invoked_count++;
    dispatch_last_session = session_key;
    dispatch_last_content = content;
    return HU_OK;
}

static void test_channel_loop_tick_dispatches_messages(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    dispatch_invoked_count = 0;
    dispatch_last_session = NULL;
    dispatch_last_content = NULL;
    hu_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .channel_ctx = NULL,
        .agent_ctx = (void *)1,
        .poll_fn = poll_return_two,
        .dispatch_fn = dispatch_count,
    };
    int processed = 0;
    hu_error_t err = hu_channel_loop_tick(&ctx, &state, &processed);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(processed, 2);
    HU_ASSERT_EQ(dispatch_invoked_count, 2);
    HU_ASSERT_STR_EQ(dispatch_last_session, "sess2");
    HU_ASSERT_STR_EQ(dispatch_last_content, "world");
}

static size_t evict_invoked;
static size_t evict_max_idle;
static size_t evict_fn_impl(void *ctx, uint64_t max_idle_secs) {
    (void)ctx;
    evict_invoked = 1;
    evict_max_idle = max_idle_secs;
    return 0;
}

static void test_channel_loop_tick_evict_called(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    evict_invoked = 0;
    evict_max_idle = 0;
    hu_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = noop_poll,
        .dispatch_fn = dispatch_echo,
        .evict_fn = evict_fn_impl,
        .evict_ctx = (void *)1,
        .idle_timeout_secs = 300,
    };
    int processed = 0;
    hu_error_t err = hu_channel_loop_tick(&ctx, &state, &processed);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(evict_invoked, 1u);
    HU_ASSERT_EQ(evict_max_idle, 300u);
}

static void test_channel_loop_tick_evict_not_called_without_timeout(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    evict_invoked = 0;
    hu_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = noop_poll,
        .dispatch_fn = dispatch_echo,
        .evict_fn = evict_fn_impl,
        .evict_ctx = (void *)1,
        .idle_timeout_secs = 0,
    };
    hu_channel_loop_tick(&ctx, &state, NULL);
    HU_ASSERT_EQ(evict_invoked, 0u);
}

static void test_channel_loop_tick_processed_null_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    hu_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = noop_poll,
        .dispatch_fn = dispatch_echo,
    };
    hu_error_t err = hu_channel_loop_tick(&ctx, &state, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

static hu_error_t poll_return_err(void *ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                                  size_t max_msgs, size_t *out_count) {
    (void)ctx;
    (void)alloc;
    (void)msgs;
    (void)max_msgs;
    (void)out_count;
    return HU_ERR_IO;
}

static void test_channel_loop_tick_poll_error_propagates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_loop_state_t state;
    hu_channel_loop_state_init(&state);
    hu_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = poll_return_err,
        .dispatch_fn = dispatch_echo,
    };
    int processed = 0;
    hu_error_t err = hu_channel_loop_tick(&ctx, &state, &processed);
    HU_ASSERT_EQ(err, HU_ERR_IO);
}

static void test_channel_loop_request_stop_null_safe(void) {
    hu_channel_loop_request_stop(NULL);
}

static void test_channel_loop_touch_null_safe(void) {
    hu_channel_loop_touch(NULL);
}

void run_channel_loop_tests(void) {
    HU_TEST_SUITE("channel_loop");
    HU_RUN_TEST(test_channel_loop_state_init);
    HU_RUN_TEST(test_channel_loop_request_stop);
    HU_RUN_TEST(test_channel_loop_should_stop_null_returns_true);
    HU_RUN_TEST(test_channel_loop_touch_updates_activity);
    HU_RUN_TEST(test_channel_loop_tick_empty_poll);
    HU_RUN_TEST(test_channel_loop_tick_null_ctx);
    HU_RUN_TEST(test_channel_loop_tick_null_state);
    HU_RUN_TEST(test_channel_loop_tick_null_alloc);
    HU_RUN_TEST(test_channel_loop_tick_null_poll_fn);
    HU_RUN_TEST(test_channel_loop_tick_null_dispatch_fn);
    HU_RUN_TEST(test_channel_loop_tick_dispatches_messages);
    HU_RUN_TEST(test_channel_loop_tick_evict_called);
    HU_RUN_TEST(test_channel_loop_tick_evict_not_called_without_timeout);
    HU_RUN_TEST(test_channel_loop_tick_processed_null_ok);
    HU_RUN_TEST(test_channel_loop_tick_poll_error_propagates);
    HU_RUN_TEST(test_channel_loop_request_stop_null_safe);
    HU_RUN_TEST(test_channel_loop_touch_null_safe);
}
