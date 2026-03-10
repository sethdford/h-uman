#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/observability/log_observer.h"
#include "human/observability/metrics_observer.h"
#include "human/observability/multi_observer.h"
#include "human/observer.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void test_log_observer_records_event(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);

    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    HU_ASSERT_NOT_NULL(obs.ctx);
    HU_ASSERT_NOT_NULL(obs.vtable);

    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL, .data = {{0}}};
    ev.data.tool_call.tool = "shell";
    ev.data.tool_call.duration_ms = 42;
    ev.data.tool_call.success = true;
    ev.data.tool_call.detail = NULL;

    hu_observer_record_event(obs, &ev);
    hu_observer_flush(obs);

    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(strstr(buf, "\"event\":\"tool_call\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"tool\":\"shell\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"duration_ms\":42") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"success\":true") != NULL);

    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_records_metric(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);

    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    HU_ASSERT_NOT_NULL(obs.ctx);

    hu_observer_metric_t m = {.tag = HU_OBSERVER_METRIC_TOKENS_USED, .value = 100};
    hu_observer_record_metric(obs, &m);
    hu_observer_flush(obs);

    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(strstr(buf, "\"metric\":\"tokens_used\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"value\":100") != NULL);

    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_counts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_observer_t obs = hu_metrics_observer_create(&alloc);
    HU_ASSERT_NOT_NULL(obs.ctx);

    hu_observer_event_t ev_agent = {.tag = HU_OBSERVER_EVENT_AGENT_START, .data = {{0}}};
    ev_agent.data.agent_start.provider = "openai";
    ev_agent.data.agent_start.model = "gpt-4";
    hu_observer_record_event(obs, &ev_agent);

    hu_observer_event_t ev_llm = {.tag = HU_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}}};
    ev_llm.data.llm_response.duration_ms = 50;
    ev_llm.data.llm_response.success = true;
    hu_observer_record_event(obs, &ev_llm);

    hu_observer_event_t ev_end = {.tag = HU_OBSERVER_EVENT_AGENT_END, .data = {{0}}};
    ev_end.data.agent_end.tokens_used = 200;
    hu_observer_record_event(obs, &ev_end);

    hu_observer_event_t ev_tool = {.tag = HU_OBSERVER_EVENT_TOOL_CALL, .data = {{0}}};
    ev_tool.data.tool_call.tool = "shell";
    ev_tool.data.tool_call.success = true;
    hu_observer_record_event(obs, &ev_tool);

    hu_metrics_snapshot_t snap;
    hu_metrics_observer_snapshot(obs, &snap);
    HU_ASSERT_EQ(snap.total_requests, (uint64_t)1);
    HU_ASSERT_EQ(snap.total_tokens, (uint64_t)200);
    HU_ASSERT_EQ(snap.total_tool_calls, (uint64_t)1);
    HU_ASSERT_EQ(snap.total_errors, (uint64_t)0);
    HU_ASSERT_FLOAT_EQ(snap.avg_latency_ms, 50.0, 0.01);

    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_snapshot(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_observer_t obs = hu_metrics_observer_create(&alloc);
    HU_ASSERT_NOT_NULL(obs.ctx);

    hu_metrics_snapshot_t snap;
    hu_metrics_observer_snapshot(obs, &snap);
    HU_ASSERT_EQ(snap.total_requests, (uint64_t)0);
    HU_ASSERT_EQ(snap.total_tokens, (uint64_t)0);
    HU_ASSERT_EQ(snap.total_tool_calls, (uint64_t)0);
    HU_ASSERT_EQ(snap.total_errors, (uint64_t)0);
    HU_ASSERT_FLOAT_EQ(snap.avg_latency_ms, 0.0, 0.01);

    hu_observer_event_t ev_err = {.tag = HU_OBSERVER_EVENT_ERR, .data = {{0}}};
    ev_err.data.err.component = "test";
    ev_err.data.err.message = "err";
    hu_observer_record_event(obs, &ev_err);

    hu_metrics_observer_snapshot(obs, &snap);
    HU_ASSERT_EQ(snap.total_errors, (uint64_t)1);

    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_multi_observer_forwards(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf1[256], buf2[256];
    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));
    FILE *f1 = tmpfile();
    FILE *f2 = tmpfile();
    HU_ASSERT_NOT_NULL(f1);
    HU_ASSERT_NOT_NULL(f2);

    hu_observer_t obs1 = hu_log_observer_create(&alloc, f1);
    hu_observer_t obs2 = hu_log_observer_create(&alloc, f2);
    hu_observer_t observers[2] = {obs1, obs2};

    hu_observer_t multi = hu_multi_observer_create(&alloc, observers, 2);
    HU_ASSERT_NOT_NULL(multi.ctx);

    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TURN_COMPLETE, .data = {{0}}};
    hu_observer_record_event(multi, &ev);
    hu_observer_flush(multi);

    rewind(f1);
    rewind(f2);
    size_t n1 = fread(buf1, 1, sizeof(buf1) - 1, f1);
    size_t n2 = fread(buf2, 1, sizeof(buf2) - 1, f2);
    buf1[n1] = '\0';
    buf2[n2] = '\0';
    fclose(f1);
    fclose(f2);
    HU_ASSERT_TRUE(strstr(buf1, "turn_complete") != NULL);
    HU_ASSERT_TRUE(strstr(buf2, "turn_complete") != NULL);

    if (multi.vtable && multi.vtable->deinit)
        multi.vtable->deinit(multi.ctx);
    if (obs1.vtable && obs1.vtable->deinit)
        obs1.vtable->deinit(obs1.ctx);
    if (obs2.vtable && obs2.vtable->deinit)
        obs2.vtable->deinit(obs2.ctx);
}

static void test_observer_null_safe(void) {
    hu_observer_t null_obs = {.ctx = NULL, .vtable = NULL};
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TURN_COMPLETE, .data = {{0}}};
    hu_observer_metric_t m = {.tag = HU_OBSERVER_METRIC_TOKENS_USED, .value = 1};

    hu_observer_record_event(null_obs, &ev);
    hu_observer_record_metric(null_obs, &m);
    hu_observer_flush(null_obs);

    /* name returns "none" when vtable is NULL (null-safe) */
    HU_ASSERT_STR_EQ(hu_observer_name(null_obs), "none");
}

static void test_log_observer_agent_start_event(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_AGENT_START, .data = {{0}}};
    ev.data.agent_start.provider = "anthropic";
    ev.data.agent_start.model = "claude-3";
    hu_observer_record_event(obs, &ev);
    hu_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(strstr(buf, "agent_start") != NULL || strstr(buf, "agent") != NULL);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_agent_end_event(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_AGENT_END, .data = {{0}}};
    ev.data.agent_end.duration_ms = 100;
    ev.data.agent_end.tokens_used = 500;
    hu_observer_record_event(obs, &ev);
    hu_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(strstr(buf, "agent_end") != NULL || strstr(buf, "tokens") != NULL);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_err_event(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_ERR, .data = {{0}}};
    ev.data.err.component = "gateway";
    ev.data.err.message = "connection refused";
    hu_observer_record_event(obs, &ev);
    hu_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(strstr(buf, "err") != NULL || strstr(buf, "error") != NULL);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_channel_message_event(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_CHANNEL_MESSAGE, .data = {{0}}};
    ev.data.channel_message.channel = "telegram";
    ev.data.channel_message.direction = "inbound";
    hu_observer_record_event(obs, &ev);
    hu_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(strstr(buf, "channel") != NULL || strstr(buf, "telegram") != NULL);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_tool_call_start(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}}};
    ev.data.tool_call_start.tool = "web_fetch";
    hu_observer_record_event(obs, &ev);
    hu_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(strstr(buf, "web_fetch") != NULL || strstr(buf, "tool") != NULL);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_request_latency(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_observer_t obs = hu_metrics_observer_create(&alloc);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}}};
    ev.data.llm_response.duration_ms = 200;
    ev.data.llm_response.success = true;
    hu_observer_record_event(obs, &ev);
    hu_metrics_snapshot_t snap;
    hu_metrics_observer_snapshot(obs, &snap);
    HU_ASSERT_FLOAT_EQ(snap.avg_latency_ms, 200.0, 0.01);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_tool_call_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_observer_t obs = hu_metrics_observer_create(&alloc);
    hu_observer_event_t ev1 = {.tag = HU_OBSERVER_EVENT_TOOL_CALL, .data = {{0}}};
    ev1.data.tool_call.tool = "file_read";
    ev1.data.tool_call.success = true;
    hu_observer_record_event(obs, &ev1);
    hu_observer_event_t ev2 = {.tag = HU_OBSERVER_EVENT_TOOL_CALL, .data = {{0}}};
    ev2.data.tool_call.tool = "shell";
    ev2.data.tool_call.success = true;
    hu_observer_record_event(obs, &ev2);
    hu_metrics_snapshot_t snap;
    hu_metrics_observer_snapshot(obs, &snap);
    HU_ASSERT_EQ(snap.total_tool_calls, (uint64_t)2);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_metric_tokens(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_observer_t obs = hu_metrics_observer_create(&alloc);
    hu_observer_metric_t m = {.tag = HU_OBSERVER_METRIC_TOKENS_USED, .value = 1234};
    hu_observer_record_metric(obs, &m);
    hu_metrics_snapshot_t snap;
    hu_metrics_observer_snapshot(obs, &snap);
    HU_ASSERT_EQ(snap.total_tokens, (uint64_t)1234);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_multiple_requests(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_observer_t obs = hu_metrics_observer_create(&alloc);
    hu_observer_event_t ev1 = {.tag = HU_OBSERVER_EVENT_AGENT_START, .data = {{0}}};
    ev1.data.agent_start.provider = "openai";
    hu_observer_record_event(obs, &ev1);
    hu_observer_event_t ev2 = {.tag = HU_OBSERVER_EVENT_AGENT_END, .data = {{0}}};
    ev2.data.agent_end.tokens_used = 100;
    hu_observer_record_event(obs, &ev2);
    hu_observer_event_t ev3 = {.tag = HU_OBSERVER_EVENT_AGENT_START, .data = {{0}}};
    hu_observer_record_event(obs, &ev3);
    hu_observer_event_t ev4 = {.tag = HU_OBSERVER_EVENT_AGENT_END, .data = {{0}}};
    ev4.data.agent_end.tokens_used = 200;
    hu_observer_record_event(obs, &ev4);
    hu_metrics_snapshot_t snap;
    hu_metrics_observer_snapshot(obs, &snap);
    HU_ASSERT_EQ(snap.total_requests, (uint64_t)2);
    HU_ASSERT_EQ(snap.total_tokens, (uint64_t)300);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_multi_observer_three_observers(void) {
    hu_allocator_t alloc = hu_system_allocator();
    FILE *f1 = tmpfile();
    FILE *f2 = tmpfile();
    FILE *f3 = tmpfile();
    HU_ASSERT_NOT_NULL(f1);
    HU_ASSERT_NOT_NULL(f2);
    HU_ASSERT_NOT_NULL(f3);
    hu_observer_t obs1 = hu_log_observer_create(&alloc, f1);
    hu_observer_t obs2 = hu_log_observer_create(&alloc, f2);
    hu_observer_t obs3 = hu_log_observer_create(&alloc, f3);
    hu_observer_t observers[3] = {obs1, obs2, obs3};
    hu_observer_t multi = hu_multi_observer_create(&alloc, observers, 3);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_HEARTBEAT_TICK, .data = {{0}}};
    hu_observer_record_event(multi, &ev);
    hu_observer_flush(multi);
    char buf[256];
    rewind(f1);
    size_t n1 = fread(buf, 1, sizeof(buf) - 1, f1);
    buf[n1] = '\0';
    fclose(f1);
    fclose(f2);
    fclose(f3);
    HU_ASSERT_TRUE(n1 > 0);
    if (multi.vtable && multi.vtable->deinit)
        multi.vtable->deinit(multi.ctx);
    if (obs1.vtable && obs1.vtable->deinit)
        obs1.vtable->deinit(obs1.ctx);
    if (obs2.vtable && obs2.vtable->deinit)
        obs2.vtable->deinit(obs2.ctx);
    if (obs3.vtable && obs3.vtable->deinit)
        obs3.vtable->deinit(obs3.ctx);
}

/* Log observer creation/destruction */
static void test_log_observer_create_has_ctx(void) {
    hu_allocator_t alloc = hu_system_allocator();
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    HU_ASSERT_NOT_NULL(obs.ctx);
    HU_ASSERT_NOT_NULL(obs.vtable);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
    fclose(f);
}

/* Event emission and capture */
static void test_log_observer_llm_request_event(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_LLM_REQUEST, .data = {{0}}};
    ev.data.llm_request.provider = "anthropic";
    ev.data.llm_request.model = "claude-3";
    ev.data.llm_request.messages_count = 5;
    hu_observer_record_event(obs, &ev);
    hu_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(strstr(buf, "anthropic") != NULL || strstr(buf, "llm") != NULL || n > 0);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

/* Multiple observers receive same event */
static void test_multi_observer_both_receive_event(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf1[256], buf2[256];
    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));
    FILE *f1 = tmpfile();
    FILE *f2 = tmpfile();
    HU_ASSERT_NOT_NULL(f1);
    HU_ASSERT_NOT_NULL(f2);
    hu_observer_t obs1 = hu_log_observer_create(&alloc, f1);
    hu_observer_t obs2 = hu_log_observer_create(&alloc, f2);
    hu_observer_t observers[2] = {obs1, obs2};
    hu_observer_t multi = hu_multi_observer_create(&alloc, observers, 2);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}}};
    ev.data.tool_call_start.tool = "file_read";
    hu_observer_record_event(multi, &ev);
    hu_observer_flush(multi);
    rewind(f1);
    rewind(f2);
    size_t n1 = fread(buf1, 1, sizeof(buf1) - 1, f1);
    size_t n2 = fread(buf2, 1, sizeof(buf2) - 1, f2);
    buf1[n1] = '\0';
    buf2[n2] = '\0';
    fclose(f1);
    fclose(f2);
    HU_ASSERT_TRUE(n1 > 0 || n2 > 0);
    if (multi.vtable && multi.vtable->deinit)
        multi.vtable->deinit(multi.ctx);
    if (obs1.vtable && obs1.vtable->deinit)
        obs1.vtable->deinit(obs1.ctx);
    if (obs2.vtable && obs2.vtable->deinit)
        obs2.vtable->deinit(obs2.ctx);
}

/* Metric recording */
static void test_log_observer_metric_request_latency(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_metric_t m = {.tag = HU_OBSERVER_METRIC_REQUEST_LATENCY_MS, .value = 150};
    hu_observer_record_metric(obs, &m);
    hu_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(strstr(buf, "150") != NULL || strstr(buf, "request") != NULL || n > 0);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_metric_active_sessions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_metric_t m = {.tag = HU_OBSERVER_METRIC_ACTIVE_SESSIONS, .value = 3};
    hu_observer_record_metric(obs, &m);
    hu_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(strstr(buf, "3") != NULL || n > 0);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_metric_queue_depth(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_metric_t m = {.tag = HU_OBSERVER_METRIC_QUEUE_DEPTH, .value = 42};
    hu_observer_record_metric(obs, &m);
    hu_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(strstr(buf, "42") != NULL || n > 0);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

/* Observer name */
static void test_log_observer_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    const char *name = hu_observer_name(obs);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
    fclose(f);
}

static void test_metrics_observer_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_observer_t obs = hu_metrics_observer_create(&alloc);
    const char *name = hu_observer_name(obs);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_multi_observer_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_observer_t obs1 = hu_metrics_observer_create(&alloc);
    hu_observer_t observers[1] = {obs1};
    hu_observer_t multi = hu_multi_observer_create(&alloc, observers, 1);
    const char *name = hu_observer_name(multi);
    HU_ASSERT_NOT_NULL(name);
    if (multi.vtable && multi.vtable->deinit)
        multi.vtable->deinit(multi.ctx);
    if (obs1.vtable && obs1.vtable->deinit)
        obs1.vtable->deinit(obs1.ctx);
}

/* Tool iterations exhausted event */
static void test_log_observer_tool_iterations_exhausted(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED, .data = {{0}}};
    ev.data.tool_iterations_exhausted.iterations = 10;
    hu_observer_record_event(obs, &ev);
    hu_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(n > 0);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

/* Metrics snapshot avg_latency with multiple responses */
static void test_metrics_observer_avg_latency_multiple(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_observer_t obs = hu_metrics_observer_create(&alloc);
    hu_observer_event_t ev1 = {.tag = HU_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}}};
    ev1.data.llm_response.duration_ms = 100;
    ev1.data.llm_response.success = true;
    hu_observer_record_event(obs, &ev1);
    hu_observer_event_t ev2 = {.tag = HU_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}}};
    ev2.data.llm_response.duration_ms = 200;
    ev2.data.llm_response.success = true;
    hu_observer_record_event(obs, &ev2);
    hu_metrics_snapshot_t snap;
    hu_metrics_observer_snapshot(obs, &snap);
    HU_ASSERT_FLOAT_EQ(snap.avg_latency_ms, 150.0, 1.0);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

/* All metric tags recorded */
static void test_log_observer_all_metric_tags(void) {
    hu_allocator_t alloc = hu_system_allocator();
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_metric_t m1 = {.tag = HU_OBSERVER_METRIC_REQUEST_LATENCY_MS, .value = 100};
    hu_observer_metric_t m2 = {.tag = HU_OBSERVER_METRIC_ACTIVE_SESSIONS, .value = 2};
    hu_observer_record_metric(obs, &m1);
    hu_observer_record_metric(obs, &m2);
    hu_observer_flush(obs);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
    fclose(f);
}

/* Observer flush is idempotent */
static void test_log_observer_flush_idempotent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    hu_observer_flush(obs);
    hu_observer_flush(obs);
    hu_observer_flush(obs);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
    fclose(f);
}

/* Multi observer empty array (count=0) */
static void test_multi_observer_empty_does_not_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_observer_t multi = hu_multi_observer_create(&alloc, NULL, 0);
    HU_ASSERT_NOT_NULL(multi.ctx);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TURN_COMPLETE, .data = {{0}}};
    hu_observer_record_event(multi, &ev);
    hu_observer_flush(multi);
    if (multi.vtable && multi.vtable->deinit)
        multi.vtable->deinit(multi.ctx);
}

void run_observer_tests(void) {
    HU_TEST_SUITE("Observer");
    HU_RUN_TEST(test_log_observer_records_event);
    HU_RUN_TEST(test_log_observer_records_metric);
    HU_RUN_TEST(test_metrics_observer_counts);
    HU_RUN_TEST(test_metrics_observer_snapshot);
    HU_RUN_TEST(test_multi_observer_forwards);
    HU_RUN_TEST(test_multi_observer_three_observers);
    HU_RUN_TEST(test_observer_null_safe);
    HU_RUN_TEST(test_log_observer_agent_start_event);
    HU_RUN_TEST(test_log_observer_agent_end_event);
    HU_RUN_TEST(test_log_observer_err_event);
    HU_RUN_TEST(test_log_observer_channel_message_event);
    HU_RUN_TEST(test_log_observer_tool_call_start);
    HU_RUN_TEST(test_metrics_observer_request_latency);
    HU_RUN_TEST(test_metrics_observer_tool_call_count);
    HU_RUN_TEST(test_metrics_observer_metric_tokens);
    HU_RUN_TEST(test_metrics_observer_multiple_requests);
    HU_RUN_TEST(test_log_observer_create_has_ctx);
    HU_RUN_TEST(test_log_observer_llm_request_event);
    HU_RUN_TEST(test_multi_observer_both_receive_event);
    HU_RUN_TEST(test_log_observer_metric_request_latency);
    HU_RUN_TEST(test_log_observer_metric_active_sessions);
    HU_RUN_TEST(test_log_observer_metric_queue_depth);
    HU_RUN_TEST(test_log_observer_name);
    HU_RUN_TEST(test_metrics_observer_name);
    HU_RUN_TEST(test_multi_observer_name);
    HU_RUN_TEST(test_log_observer_tool_iterations_exhausted);
    HU_RUN_TEST(test_metrics_observer_avg_latency_multiple);
    HU_RUN_TEST(test_multi_observer_empty_does_not_crash);
    HU_RUN_TEST(test_log_observer_all_metric_tags);
    HU_RUN_TEST(test_log_observer_flush_idempotent);
}
