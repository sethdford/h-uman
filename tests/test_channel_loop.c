#include "test_framework.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>

static void test_channel_loop_state_init(void) {
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    SC_ASSERT_FALSE(sc_channel_loop_should_stop(&state));
}

static void test_channel_loop_request_stop(void) {
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    sc_channel_loop_request_stop(&state);
    SC_ASSERT_TRUE(sc_channel_loop_should_stop(&state));
}

static void test_channel_loop_should_stop_null_returns_true(void) {
    SC_ASSERT_TRUE(sc_channel_loop_should_stop(NULL));
}

static void test_channel_loop_touch_updates_activity(void) {
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    sc_channel_loop_touch(&state);
    SC_ASSERT(state.last_activity > 0);
}

static sc_error_t noop_poll(void *ctx, sc_allocator_t *alloc,
    sc_channel_loop_msg_t *msgs, size_t max_msgs, size_t *out_count) {
    (void)ctx;
    (void)alloc;
    (void)msgs;
    (void)max_msgs;
    *out_count = 0;
    return SC_OK;
}

static sc_error_t dispatch_echo(void *ctx, const char *session_key,
    const char *content, char **response_out) {
    (void)ctx;
    (void)session_key;
    (void)content;
    *response_out = NULL;
    return SC_OK;
}

static void test_channel_loop_tick_empty_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    sc_channel_loop_ctx_t ctx = {
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
    sc_error_t err = sc_channel_loop_tick(&ctx, &state, &processed);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(processed, 0);
}

static void test_channel_loop_tick_null_ctx(void) {
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    int processed = 0;
    sc_error_t err = sc_channel_loop_tick(NULL, &state, &processed);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_channel_loop_tick_null_state(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = noop_poll,
        .dispatch_fn = dispatch_echo,
    };
    int processed = 0;
    sc_error_t err = sc_channel_loop_tick(&ctx, NULL, &processed);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_channel_loop_tick_null_alloc(void) {
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    sc_channel_loop_ctx_t ctx = {
        .alloc = NULL,
        .channel_ctx = NULL,
        .agent_ctx = NULL,
        .poll_fn = noop_poll,
        .dispatch_fn = dispatch_echo,
    };
    int processed = 0;
    sc_error_t err = sc_channel_loop_tick(&ctx, &state, &processed);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_channel_loop_tick_null_poll_fn(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    sc_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = NULL,
        .dispatch_fn = dispatch_echo,
    };
    int processed = 0;
    sc_error_t err = sc_channel_loop_tick(&ctx, &state, &processed);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_channel_loop_tick_null_dispatch_fn(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    sc_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = noop_poll,
        .dispatch_fn = NULL,
    };
    int processed = 0;
    sc_error_t err = sc_channel_loop_tick(&ctx, &state, &processed);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static sc_error_t poll_return_two(void *ctx, sc_allocator_t *alloc,
    sc_channel_loop_msg_t *msgs, size_t max_msgs, size_t *out_count) {
    (void)ctx;
    (void)alloc;
    if (max_msgs < 2) { *out_count = 0; return SC_OK; }
    strncpy(msgs[0].session_key, "sess1", sizeof(msgs[0].session_key) - 1);
    msgs[0].session_key[sizeof(msgs[0].session_key) - 1] = '\0';
    strncpy(msgs[0].content, "hello", sizeof(msgs[0].content) - 1);
    msgs[0].content[sizeof(msgs[0].content) - 1] = '\0';
    strncpy(msgs[1].session_key, "sess2", sizeof(msgs[1].session_key) - 1);
    msgs[1].session_key[sizeof(msgs[1].session_key) - 1] = '\0';
    strncpy(msgs[1].content, "world", sizeof(msgs[1].content) - 1);
    msgs[1].content[sizeof(msgs[1].content) - 1] = '\0';
    *out_count = 2;
    return SC_OK;
}

static int dispatch_invoked_count;
static const char *dispatch_last_session;
static const char *dispatch_last_content;
static sc_error_t dispatch_count(void *ctx, const char *session_key,
    const char *content, char **response_out) {
    (void)ctx;
    *response_out = NULL;
    dispatch_invoked_count++;
    dispatch_last_session = session_key;
    dispatch_last_content = content;
    return SC_OK;
}

static void test_channel_loop_tick_dispatches_messages(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    dispatch_invoked_count = 0;
    dispatch_last_session = NULL;
    dispatch_last_content = NULL;
    sc_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .channel_ctx = NULL,
        .agent_ctx = (void *)1,
        .poll_fn = poll_return_two,
        .dispatch_fn = dispatch_count,
    };
    int processed = 0;
    sc_error_t err = sc_channel_loop_tick(&ctx, &state, &processed);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(processed, 2);
    SC_ASSERT_EQ(dispatch_invoked_count, 2);
    SC_ASSERT_STR_EQ(dispatch_last_session, "sess2");
    SC_ASSERT_STR_EQ(dispatch_last_content, "world");
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
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    evict_invoked = 0;
    evict_max_idle = 0;
    sc_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = noop_poll,
        .dispatch_fn = dispatch_echo,
        .evict_fn = evict_fn_impl,
        .evict_ctx = (void *)1,
        .idle_timeout_secs = 300,
    };
    int processed = 0;
    sc_error_t err = sc_channel_loop_tick(&ctx, &state, &processed);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(evict_invoked, 1u);
    SC_ASSERT_EQ(evict_max_idle, 300u);
}

static void test_channel_loop_tick_evict_not_called_without_timeout(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    evict_invoked = 0;
    sc_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = noop_poll,
        .dispatch_fn = dispatch_echo,
        .evict_fn = evict_fn_impl,
        .evict_ctx = (void *)1,
        .idle_timeout_secs = 0,
    };
    sc_channel_loop_tick(&ctx, &state, NULL);
    SC_ASSERT_EQ(evict_invoked, 0u);
}

static void test_channel_loop_tick_processed_null_ok(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    sc_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = noop_poll,
        .dispatch_fn = dispatch_echo,
    };
    sc_error_t err = sc_channel_loop_tick(&ctx, &state, NULL);
    SC_ASSERT_EQ(err, SC_OK);
}

static sc_error_t poll_return_err(void *ctx, sc_allocator_t *alloc,
    sc_channel_loop_msg_t *msgs, size_t max_msgs, size_t *out_count) {
    (void)ctx;
    (void)alloc;
    (void)msgs;
    (void)max_msgs;
    (void)out_count;
    return SC_ERR_IO;
}

static void test_channel_loop_tick_poll_error_propagates(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_loop_state_t state;
    sc_channel_loop_state_init(&state);
    sc_channel_loop_ctx_t ctx = {
        .alloc = &alloc,
        .poll_fn = poll_return_err,
        .dispatch_fn = dispatch_echo,
    };
    int processed = 0;
    sc_error_t err = sc_channel_loop_tick(&ctx, &state, &processed);
    SC_ASSERT_EQ(err, SC_ERR_IO);
}

static void test_channel_loop_request_stop_null_safe(void) {
    sc_channel_loop_request_stop(NULL);
}

static void test_channel_loop_touch_null_safe(void) {
    sc_channel_loop_touch(NULL);
}

void run_channel_loop_tests(void) {
    SC_TEST_SUITE("channel_loop");
    SC_RUN_TEST(test_channel_loop_state_init);
    SC_RUN_TEST(test_channel_loop_request_stop);
    SC_RUN_TEST(test_channel_loop_should_stop_null_returns_true);
    SC_RUN_TEST(test_channel_loop_touch_updates_activity);
    SC_RUN_TEST(test_channel_loop_tick_empty_poll);
    SC_RUN_TEST(test_channel_loop_tick_null_ctx);
    SC_RUN_TEST(test_channel_loop_tick_null_state);
    SC_RUN_TEST(test_channel_loop_tick_null_alloc);
    SC_RUN_TEST(test_channel_loop_tick_null_poll_fn);
    SC_RUN_TEST(test_channel_loop_tick_null_dispatch_fn);
    SC_RUN_TEST(test_channel_loop_tick_dispatches_messages);
    SC_RUN_TEST(test_channel_loop_tick_evict_called);
    SC_RUN_TEST(test_channel_loop_tick_evict_not_called_without_timeout);
    SC_RUN_TEST(test_channel_loop_tick_processed_null_ok);
    SC_RUN_TEST(test_channel_loop_tick_poll_error_propagates);
    SC_RUN_TEST(test_channel_loop_request_stop_null_safe);
    SC_RUN_TEST(test_channel_loop_touch_null_safe);
}
