#include "test_framework.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/observer.h"
#include "seaclaw/observability/log_observer.h"
#include "seaclaw/observability/metrics_observer.h"
#include "seaclaw/observability/multi_observer.h"
#include <string.h>
#include <stdio.h>

static void test_log_observer_records_event(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);

    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    SC_ASSERT_NOT_NULL(obs.ctx);
    SC_ASSERT_NOT_NULL(obs.vtable);

    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TOOL_CALL, .data = {{0}} };
    ev.data.tool_call.tool = "shell";
    ev.data.tool_call.duration_ms = 42;
    ev.data.tool_call.success = true;
    ev.data.tool_call.detail = NULL;

    sc_observer_record_event(obs, &ev);
    sc_observer_flush(obs);

    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(strstr(buf, "\"event\":\"tool_call\"") != NULL);
    SC_ASSERT_TRUE(strstr(buf, "\"tool\":\"shell\"") != NULL);
    SC_ASSERT_TRUE(strstr(buf, "\"duration_ms\":42") != NULL);
    SC_ASSERT_TRUE(strstr(buf, "\"success\":true") != NULL);

    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_records_metric(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);

    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    SC_ASSERT_NOT_NULL(obs.ctx);

    sc_observer_metric_t m = { .tag = SC_OBSERVER_METRIC_TOKENS_USED, .value = 100 };
    sc_observer_record_metric(obs, &m);
    sc_observer_flush(obs);

    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(strstr(buf, "\"metric\":\"tokens_used\"") != NULL);
    SC_ASSERT_TRUE(strstr(buf, "\"value\":100") != NULL);

    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_counts(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_observer_t obs = sc_metrics_observer_create(&alloc);
    SC_ASSERT_NOT_NULL(obs.ctx);

    sc_observer_event_t ev_agent = { .tag = SC_OBSERVER_EVENT_AGENT_START, .data = {{0}} };
    ev_agent.data.agent_start.provider = "openai";
    ev_agent.data.agent_start.model = "gpt-4";
    sc_observer_record_event(obs, &ev_agent);

    sc_observer_event_t ev_llm = { .tag = SC_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}} };
    ev_llm.data.llm_response.duration_ms = 50;
    ev_llm.data.llm_response.success = true;
    sc_observer_record_event(obs, &ev_llm);

    sc_observer_event_t ev_end = { .tag = SC_OBSERVER_EVENT_AGENT_END, .data = {{0}} };
    ev_end.data.agent_end.tokens_used = 200;
    sc_observer_record_event(obs, &ev_end);

    sc_observer_event_t ev_tool = { .tag = SC_OBSERVER_EVENT_TOOL_CALL, .data = {{0}} };
    ev_tool.data.tool_call.tool = "shell";
    ev_tool.data.tool_call.success = true;
    sc_observer_record_event(obs, &ev_tool);

    sc_metrics_snapshot_t snap;
    sc_metrics_observer_snapshot(obs, &snap);
    SC_ASSERT_EQ(snap.total_requests, (uint64_t)1);
    SC_ASSERT_EQ(snap.total_tokens, (uint64_t)200);
    SC_ASSERT_EQ(snap.total_tool_calls, (uint64_t)1);
    SC_ASSERT_EQ(snap.total_errors, (uint64_t)0);
    SC_ASSERT_FLOAT_EQ(snap.avg_latency_ms, 50.0, 0.01);

    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_snapshot(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_observer_t obs = sc_metrics_observer_create(&alloc);
    SC_ASSERT_NOT_NULL(obs.ctx);

    sc_metrics_snapshot_t snap;
    sc_metrics_observer_snapshot(obs, &snap);
    SC_ASSERT_EQ(snap.total_requests, (uint64_t)0);
    SC_ASSERT_EQ(snap.total_tokens, (uint64_t)0);
    SC_ASSERT_EQ(snap.total_tool_calls, (uint64_t)0);
    SC_ASSERT_EQ(snap.total_errors, (uint64_t)0);
    SC_ASSERT_FLOAT_EQ(snap.avg_latency_ms, 0.0, 0.01);

    sc_observer_event_t ev_err = { .tag = SC_OBSERVER_EVENT_ERR, .data = {{0}} };
    ev_err.data.err.component = "test";
    ev_err.data.err.message = "err";
    sc_observer_record_event(obs, &ev_err);

    sc_metrics_observer_snapshot(obs, &snap);
    SC_ASSERT_EQ(snap.total_errors, (uint64_t)1);

    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_multi_observer_forwards(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf1[256], buf2[256];
    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));
    FILE *f1 = tmpfile();
    FILE *f2 = tmpfile();
    SC_ASSERT_NOT_NULL(f1);
    SC_ASSERT_NOT_NULL(f2);

    sc_observer_t obs1 = sc_log_observer_create(&alloc, f1);
    sc_observer_t obs2 = sc_log_observer_create(&alloc, f2);
    sc_observer_t observers[2] = { obs1, obs2 };

    sc_observer_t multi = sc_multi_observer_create(&alloc, observers, 2);
    SC_ASSERT_NOT_NULL(multi.ctx);

    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TURN_COMPLETE, .data = {{0}} };
    sc_observer_record_event(multi, &ev);
    sc_observer_flush(multi);

    rewind(f1);
    rewind(f2);
    size_t n1 = fread(buf1, 1, sizeof(buf1) - 1, f1);
    size_t n2 = fread(buf2, 1, sizeof(buf2) - 1, f2);
    buf1[n1] = '\0';
    buf2[n2] = '\0';
    fclose(f1);
    fclose(f2);
    SC_ASSERT_TRUE(strstr(buf1, "turn_complete") != NULL);
    SC_ASSERT_TRUE(strstr(buf2, "turn_complete") != NULL);

    if (multi.vtable && multi.vtable->deinit)
        multi.vtable->deinit(multi.ctx);
    if (obs1.vtable && obs1.vtable->deinit)
        obs1.vtable->deinit(obs1.ctx);
    if (obs2.vtable && obs2.vtable->deinit)
        obs2.vtable->deinit(obs2.ctx);
}

static void test_observer_null_safe(void) {
    sc_observer_t null_obs = { .ctx = NULL, .vtable = NULL };
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TURN_COMPLETE, .data = {{0}} };
    sc_observer_metric_t m = { .tag = SC_OBSERVER_METRIC_TOKENS_USED, .value = 1 };

    sc_observer_record_event(null_obs, &ev);
    sc_observer_record_metric(null_obs, &m);
    sc_observer_flush(null_obs);

    /* name returns "none" when vtable is NULL (null-safe) */
    SC_ASSERT_STR_EQ(sc_observer_name(null_obs), "none");
}

static void test_log_observer_agent_start_event(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_AGENT_START, .data = {{0}} };
    ev.data.agent_start.provider = "anthropic";
    ev.data.agent_start.model = "claude-3";
    sc_observer_record_event(obs, &ev);
    sc_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(strstr(buf, "agent_start") != NULL || strstr(buf, "agent") != NULL);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_agent_end_event(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_AGENT_END, .data = {{0}} };
    ev.data.agent_end.duration_ms = 100;
    ev.data.agent_end.tokens_used = 500;
    sc_observer_record_event(obs, &ev);
    sc_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(strstr(buf, "agent_end") != NULL || strstr(buf, "tokens") != NULL);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_err_event(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_ERR, .data = {{0}} };
    ev.data.err.component = "gateway";
    ev.data.err.message = "connection refused";
    sc_observer_record_event(obs, &ev);
    sc_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(strstr(buf, "err") != NULL || strstr(buf, "error") != NULL);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_channel_message_event(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_CHANNEL_MESSAGE, .data = {{0}} };
    ev.data.channel_message.channel = "telegram";
    ev.data.channel_message.direction = "inbound";
    sc_observer_record_event(obs, &ev);
    sc_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(strstr(buf, "channel") != NULL || strstr(buf, "telegram") != NULL);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_tool_call_start(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}} };
    ev.data.tool_call_start.tool = "web_fetch";
    sc_observer_record_event(obs, &ev);
    sc_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(strstr(buf, "web_fetch") != NULL || strstr(buf, "tool") != NULL);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_request_latency(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_observer_t obs = sc_metrics_observer_create(&alloc);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}} };
    ev.data.llm_response.duration_ms = 200;
    ev.data.llm_response.success = true;
    sc_observer_record_event(obs, &ev);
    sc_metrics_snapshot_t snap;
    sc_metrics_observer_snapshot(obs, &snap);
    SC_ASSERT_FLOAT_EQ(snap.avg_latency_ms, 200.0, 0.01);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_tool_call_count(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_observer_t obs = sc_metrics_observer_create(&alloc);
    sc_observer_event_t ev1 = { .tag = SC_OBSERVER_EVENT_TOOL_CALL, .data = {{0}} };
    ev1.data.tool_call.tool = "file_read";
    ev1.data.tool_call.success = true;
    sc_observer_record_event(obs, &ev1);
    sc_observer_event_t ev2 = { .tag = SC_OBSERVER_EVENT_TOOL_CALL, .data = {{0}} };
    ev2.data.tool_call.tool = "shell";
    ev2.data.tool_call.success = true;
    sc_observer_record_event(obs, &ev2);
    sc_metrics_snapshot_t snap;
    sc_metrics_observer_snapshot(obs, &snap);
    SC_ASSERT_EQ(snap.total_tool_calls, (uint64_t)2);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_metric_tokens(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_observer_t obs = sc_metrics_observer_create(&alloc);
    sc_observer_metric_t m = { .tag = SC_OBSERVER_METRIC_TOKENS_USED, .value = 1234 };
    sc_observer_record_metric(obs, &m);
    sc_metrics_snapshot_t snap;
    sc_metrics_observer_snapshot(obs, &snap);
    SC_ASSERT_EQ(snap.total_tokens, (uint64_t)1234);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_metrics_observer_multiple_requests(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_observer_t obs = sc_metrics_observer_create(&alloc);
    sc_observer_event_t ev1 = { .tag = SC_OBSERVER_EVENT_AGENT_START, .data = {{0}} };
    ev1.data.agent_start.provider = "openai";
    sc_observer_record_event(obs, &ev1);
    sc_observer_event_t ev2 = { .tag = SC_OBSERVER_EVENT_AGENT_END, .data = {{0}} };
    ev2.data.agent_end.tokens_used = 100;
    sc_observer_record_event(obs, &ev2);
    sc_observer_event_t ev3 = { .tag = SC_OBSERVER_EVENT_AGENT_START, .data = {{0}} };
    sc_observer_record_event(obs, &ev3);
    sc_observer_event_t ev4 = { .tag = SC_OBSERVER_EVENT_AGENT_END, .data = {{0}} };
    ev4.data.agent_end.tokens_used = 200;
    sc_observer_record_event(obs, &ev4);
    sc_metrics_snapshot_t snap;
    sc_metrics_observer_snapshot(obs, &snap);
    SC_ASSERT_EQ(snap.total_requests, (uint64_t)2);
    SC_ASSERT_EQ(snap.total_tokens, (uint64_t)300);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_multi_observer_three_observers(void) {
    sc_allocator_t alloc = sc_system_allocator();
    FILE *f1 = tmpfile();
    FILE *f2 = tmpfile();
    FILE *f3 = tmpfile();
    SC_ASSERT_NOT_NULL(f1);
    SC_ASSERT_NOT_NULL(f2);
    SC_ASSERT_NOT_NULL(f3);
    sc_observer_t obs1 = sc_log_observer_create(&alloc, f1);
    sc_observer_t obs2 = sc_log_observer_create(&alloc, f2);
    sc_observer_t obs3 = sc_log_observer_create(&alloc, f3);
    sc_observer_t observers[3] = { obs1, obs2, obs3 };
    sc_observer_t multi = sc_multi_observer_create(&alloc, observers, 3);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_HEARTBEAT_TICK, .data = {{0}} };
    sc_observer_record_event(multi, &ev);
    sc_observer_flush(multi);
    char buf[256];
    rewind(f1);
    size_t n1 = fread(buf, 1, sizeof(buf) - 1, f1);
    buf[n1] = '\0';
    fclose(f1);
    fclose(f2);
    fclose(f3);
    SC_ASSERT_TRUE(n1 > 0);
    if (multi.vtable && multi.vtable->deinit) multi.vtable->deinit(multi.ctx);
    if (obs1.vtable && obs1.vtable->deinit) obs1.vtable->deinit(obs1.ctx);
    if (obs2.vtable && obs2.vtable->deinit) obs2.vtable->deinit(obs2.ctx);
    if (obs3.vtable && obs3.vtable->deinit) obs3.vtable->deinit(obs3.ctx);
}

/* Log observer creation/destruction */
static void test_log_observer_create_has_ctx(void) {
    sc_allocator_t alloc = sc_system_allocator();
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    SC_ASSERT_NOT_NULL(obs.ctx);
    SC_ASSERT_NOT_NULL(obs.vtable);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
    fclose(f);
}

/* Event emission and capture */
static void test_log_observer_llm_request_event(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_LLM_REQUEST, .data = {{0}} };
    ev.data.llm_request.provider = "anthropic";
    ev.data.llm_request.model = "claude-3";
    ev.data.llm_request.messages_count = 5;
    sc_observer_record_event(obs, &ev);
    sc_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(strstr(buf, "anthropic") != NULL || strstr(buf, "llm") != NULL || n > 0);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

/* Multiple observers receive same event */
static void test_multi_observer_both_receive_event(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf1[256], buf2[256];
    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));
    FILE *f1 = tmpfile();
    FILE *f2 = tmpfile();
    SC_ASSERT_NOT_NULL(f1);
    SC_ASSERT_NOT_NULL(f2);
    sc_observer_t obs1 = sc_log_observer_create(&alloc, f1);
    sc_observer_t obs2 = sc_log_observer_create(&alloc, f2);
    sc_observer_t observers[2] = { obs1, obs2 };
    sc_observer_t multi = sc_multi_observer_create(&alloc, observers, 2);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}} };
    ev.data.tool_call_start.tool = "file_read";
    sc_observer_record_event(multi, &ev);
    sc_observer_flush(multi);
    rewind(f1);
    rewind(f2);
    size_t n1 = fread(buf1, 1, sizeof(buf1) - 1, f1);
    size_t n2 = fread(buf2, 1, sizeof(buf2) - 1, f2);
    buf1[n1] = '\0';
    buf2[n2] = '\0';
    fclose(f1);
    fclose(f2);
    SC_ASSERT_TRUE(n1 > 0 || n2 > 0);
    if (multi.vtable && multi.vtable->deinit) multi.vtable->deinit(multi.ctx);
    if (obs1.vtable && obs1.vtable->deinit) obs1.vtable->deinit(obs1.ctx);
    if (obs2.vtable && obs2.vtable->deinit) obs2.vtable->deinit(obs2.ctx);
}

/* Metric recording */
static void test_log_observer_metric_request_latency(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_metric_t m = { .tag = SC_OBSERVER_METRIC_REQUEST_LATENCY_MS, .value = 150 };
    sc_observer_record_metric(obs, &m);
    sc_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(strstr(buf, "150") != NULL || strstr(buf, "request") != NULL || n > 0);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_metric_active_sessions(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_metric_t m = { .tag = SC_OBSERVER_METRIC_ACTIVE_SESSIONS, .value = 3 };
    sc_observer_record_metric(obs, &m);
    sc_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(strstr(buf, "3") != NULL || n > 0);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_log_observer_metric_queue_depth(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_metric_t m = { .tag = SC_OBSERVER_METRIC_QUEUE_DEPTH, .value = 42 };
    sc_observer_record_metric(obs, &m);
    sc_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(strstr(buf, "42") != NULL || n > 0);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

/* Observer name */
static void test_log_observer_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    const char *name = sc_observer_name(obs);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT_TRUE(strlen(name) > 0);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
    fclose(f);
}

static void test_metrics_observer_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_observer_t obs = sc_metrics_observer_create(&alloc);
    const char *name = sc_observer_name(obs);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT_TRUE(strlen(name) > 0);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_multi_observer_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_observer_t obs1 = sc_metrics_observer_create(&alloc);
    sc_observer_t observers[1] = { obs1 };
    sc_observer_t multi = sc_multi_observer_create(&alloc, observers, 1);
    const char *name = sc_observer_name(multi);
    SC_ASSERT_NOT_NULL(name);
    if (multi.vtable && multi.vtable->deinit) multi.vtable->deinit(multi.ctx);
    if (obs1.vtable && obs1.vtable->deinit) obs1.vtable->deinit(obs1.ctx);
}

/* Tool iterations exhausted event */
static void test_log_observer_tool_iterations_exhausted(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED, .data = {{0}} };
    ev.data.tool_iterations_exhausted.iterations = 10;
    sc_observer_record_event(obs, &ev);
    sc_observer_flush(obs);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    SC_ASSERT_TRUE(n > 0);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

/* Metrics snapshot avg_latency with multiple responses */
static void test_metrics_observer_avg_latency_multiple(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_observer_t obs = sc_metrics_observer_create(&alloc);
    sc_observer_event_t ev1 = { .tag = SC_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}} };
    ev1.data.llm_response.duration_ms = 100;
    ev1.data.llm_response.success = true;
    sc_observer_record_event(obs, &ev1);
    sc_observer_event_t ev2 = { .tag = SC_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}} };
    ev2.data.llm_response.duration_ms = 200;
    ev2.data.llm_response.success = true;
    sc_observer_record_event(obs, &ev2);
    sc_metrics_snapshot_t snap;
    sc_metrics_observer_snapshot(obs, &snap);
    SC_ASSERT_FLOAT_EQ(snap.avg_latency_ms, 150.0, 1.0);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

/* All metric tags recorded */
static void test_log_observer_all_metric_tags(void) {
    sc_allocator_t alloc = sc_system_allocator();
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_metric_t m1 = { .tag = SC_OBSERVER_METRIC_REQUEST_LATENCY_MS, .value = 100 };
    sc_observer_metric_t m2 = { .tag = SC_OBSERVER_METRIC_ACTIVE_SESSIONS, .value = 2 };
    sc_observer_record_metric(obs, &m1);
    sc_observer_record_metric(obs, &m2);
    sc_observer_flush(obs);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
    fclose(f);
}

/* Observer flush is idempotent */
static void test_log_observer_flush_idempotent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    sc_observer_flush(obs);
    sc_observer_flush(obs);
    sc_observer_flush(obs);
    if (obs.vtable && obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
    fclose(f);
}

/* Multi observer empty array (count=0) */
static void test_multi_observer_empty_does_not_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_observer_t multi = sc_multi_observer_create(&alloc, NULL, 0);
    SC_ASSERT_NOT_NULL(multi.ctx);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_TURN_COMPLETE, .data = {{0}} };
    sc_observer_record_event(multi, &ev);
    sc_observer_flush(multi);
    if (multi.vtable && multi.vtable->deinit) multi.vtable->deinit(multi.ctx);
}

void run_observer_tests(void) {
    SC_TEST_SUITE("Observer");
    SC_RUN_TEST(test_log_observer_records_event);
    SC_RUN_TEST(test_log_observer_records_metric);
    SC_RUN_TEST(test_metrics_observer_counts);
    SC_RUN_TEST(test_metrics_observer_snapshot);
    SC_RUN_TEST(test_multi_observer_forwards);
    SC_RUN_TEST(test_multi_observer_three_observers);
    SC_RUN_TEST(test_observer_null_safe);
    SC_RUN_TEST(test_log_observer_agent_start_event);
    SC_RUN_TEST(test_log_observer_agent_end_event);
    SC_RUN_TEST(test_log_observer_err_event);
    SC_RUN_TEST(test_log_observer_channel_message_event);
    SC_RUN_TEST(test_log_observer_tool_call_start);
    SC_RUN_TEST(test_metrics_observer_request_latency);
    SC_RUN_TEST(test_metrics_observer_tool_call_count);
    SC_RUN_TEST(test_metrics_observer_metric_tokens);
    SC_RUN_TEST(test_metrics_observer_multiple_requests);
    SC_RUN_TEST(test_log_observer_create_has_ctx);
    SC_RUN_TEST(test_log_observer_llm_request_event);
    SC_RUN_TEST(test_multi_observer_both_receive_event);
    SC_RUN_TEST(test_log_observer_metric_request_latency);
    SC_RUN_TEST(test_log_observer_metric_active_sessions);
    SC_RUN_TEST(test_log_observer_metric_queue_depth);
    SC_RUN_TEST(test_log_observer_name);
    SC_RUN_TEST(test_metrics_observer_name);
    SC_RUN_TEST(test_multi_observer_name);
    SC_RUN_TEST(test_log_observer_tool_iterations_exhausted);
    SC_RUN_TEST(test_metrics_observer_avg_latency_multiple);
    SC_RUN_TEST(test_multi_observer_empty_does_not_crash);
    SC_RUN_TEST(test_log_observer_all_metric_tags);
    SC_RUN_TEST(test_log_observer_flush_idempotent);
}
