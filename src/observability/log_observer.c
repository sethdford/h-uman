#include "seaclaw/observability/log_observer.h"
#include "seaclaw/observer.h"
#include "seaclaw/core/error.h"
#include "seaclaw/providers/scrub.h"
#include <string.h>
#include <time.h>

#define SC_LOG_JSON_STR_MAX 512
#define SC_STR(s) ((s) ? (s) : "")

typedef struct sc_log_observer_ctx {
    FILE *output;
    sc_allocator_t *alloc;
    char ts_buf[32];
} sc_log_observer_ctx_t;

static void json_escape_fp(FILE *f, const char *s) {
    if (!s) return;
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
            case '"': fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default: fputc(*s, f); break;
        }
    }
    fputc('"', f);
}

static void format_ts_utc(char *buf, size_t cap) {
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void log_record_event(void *ctx, const sc_observer_event_t *event) {
    sc_log_observer_ctx_t *c = (sc_log_observer_ctx_t *)ctx;
    FILE *f = c->output ? c->output : stderr;
    format_ts_utc(c->ts_buf, sizeof(c->ts_buf));

    fprintf(f, "{\"ts\":\"%s\",\"event\":\"", c->ts_buf);

    switch (event->tag) {
        case SC_OBSERVER_EVENT_AGENT_START:
            fprintf(f, "agent_start\",\"provider\":");
            json_escape_fp(f, event->data.agent_start.provider);
            fprintf(f, ",\"model\":");
            json_escape_fp(f, event->data.agent_start.model);
            fprintf(f, "}\n");
            break;
        case SC_OBSERVER_EVENT_LLM_REQUEST:
            fprintf(f, "llm_request\",\"provider\":");
            json_escape_fp(f, event->data.llm_request.provider);
            fprintf(f, ",\"model\":");
            json_escape_fp(f, event->data.llm_request.model);
            fprintf(f, ",\"messages_count\":%zu}\n", event->data.llm_request.messages_count);
            break;
        case SC_OBSERVER_EVENT_LLM_RESPONSE: {
            fprintf(f, "llm_response\",\"provider\":");
            json_escape_fp(f, event->data.llm_response.provider);
            fprintf(f, ",\"model\":");
            json_escape_fp(f, event->data.llm_response.model);
            fprintf(f, ",\"duration_ms\":%llu,\"success\":%s",
                (unsigned long long)event->data.llm_response.duration_ms,
                event->data.llm_response.success ? "true" : "false");
            if (event->data.llm_response.error_message && event->data.llm_response.error_message[0]) {
                const char *err = event->data.llm_response.error_message;
                size_t elen = strlen(err);
                char *scrubbed = sc_scrub_sanitize_api_error(c->alloc, err, elen);
                fprintf(f, ",\"error\":");
                if (scrubbed) {
                    json_escape_fp(f, scrubbed);
                    c->alloc->free(c->alloc->ctx, scrubbed, strlen(scrubbed) + 1);
                } else {
                    char safe_error[201];
                    if (elen > 200) elen = 200;
                    memcpy(safe_error, err, elen);
                    safe_error[elen] = '\0';
                    json_escape_fp(f, safe_error);
                }
            }
            fprintf(f, "}\n");
            break;
        }
        case SC_OBSERVER_EVENT_AGENT_END:
            fprintf(f, "agent_end\",\"duration_ms\":%llu,\"tokens_used\":%llu}\n",
                (unsigned long long)event->data.agent_end.duration_ms,
                (unsigned long long)event->data.agent_end.tokens_used);
            break;
        case SC_OBSERVER_EVENT_TOOL_CALL_START:
            fprintf(f, "tool_call_start\",\"tool\":");
            json_escape_fp(f, event->data.tool_call_start.tool);
            fprintf(f, "}\n");
            break;
        case SC_OBSERVER_EVENT_TOOL_CALL: {
            fprintf(f, "tool_call\",\"tool\":");
            json_escape_fp(f, event->data.tool_call.tool);
            fprintf(f, ",\"duration_ms\":%llu,\"success\":%s",
                (unsigned long long)event->data.tool_call.duration_ms,
                event->data.tool_call.success ? "true" : "false");
            if (event->data.tool_call.detail && event->data.tool_call.detail[0]) {
                const char *det = event->data.tool_call.detail;
                size_t dlen = strlen(det);
                char *scrubbed = sc_scrub_sanitize_api_error(c->alloc, det, dlen);
                fprintf(f, ",\"detail\":");
                if (scrubbed) {
                    json_escape_fp(f, scrubbed);
                    c->alloc->free(c->alloc->ctx, scrubbed, strlen(scrubbed) + 1);
                } else {
                    char safe_detail[201];
                    if (dlen > 200) dlen = 200;
                    memcpy(safe_detail, det, dlen);
                    safe_detail[dlen] = '\0';
                    json_escape_fp(f, safe_detail);
                }
            }
            fprintf(f, "}\n");
            break;
        }
        case SC_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED:
            fprintf(f, "tool_iterations_exhausted\",\"iterations\":%u}\n",
                event->data.tool_iterations_exhausted.iterations);
            break;
        case SC_OBSERVER_EVENT_TURN_COMPLETE:
            fprintf(f, "turn_complete\"}\n");
            break;
        case SC_OBSERVER_EVENT_CHANNEL_MESSAGE:
            fprintf(f, "channel_message\",\"channel\":");
            json_escape_fp(f, event->data.channel_message.channel);
            fprintf(f, ",\"direction\":");
            json_escape_fp(f, event->data.channel_message.direction);
            fprintf(f, "}\n");
            break;
        case SC_OBSERVER_EVENT_HEARTBEAT_TICK:
            fprintf(f, "heartbeat_tick\"}\n");
            break;
        case SC_OBSERVER_EVENT_ERR:
            fprintf(f, "err\",\"component\":");
            json_escape_fp(f, event->data.err.component);
            fprintf(f, ",\"message\":");
            json_escape_fp(f, event->data.err.message);
            fprintf(f, "}\n");
            break;
    }
}

static void log_record_metric(void *ctx, const sc_observer_metric_t *metric) {
    sc_log_observer_ctx_t *c = (sc_log_observer_ctx_t *)ctx;
    FILE *f = c->output ? c->output : stderr;
    format_ts_utc(c->ts_buf, sizeof(c->ts_buf));

    const char *name = "unknown";
    switch (metric->tag) {
        case SC_OBSERVER_METRIC_REQUEST_LATENCY_MS: name = "request_latency_ms"; break;
        case SC_OBSERVER_METRIC_TOKENS_USED: name = "tokens_used"; break;
        case SC_OBSERVER_METRIC_ACTIVE_SESSIONS: name = "active_sessions"; break;
        case SC_OBSERVER_METRIC_QUEUE_DEPTH: name = "queue_depth"; break;
    }
    fprintf(f, "{\"ts\":\"%s\",\"metric\":\"%s\",\"value\":%llu}\n",
        c->ts_buf, name, (unsigned long long)metric->value);
}

static void log_flush(void *ctx) {
    sc_log_observer_ctx_t *c = (sc_log_observer_ctx_t *)ctx;
    FILE *f = c->output ? c->output : stderr;
    fflush(f);
}

static const char *log_name(void *ctx) {
    (void)ctx;
    return "log";
}

static void log_deinit(void *ctx) {
    if (!ctx) return;
    sc_log_observer_ctx_t *c = (sc_log_observer_ctx_t *)ctx;
    sc_allocator_t *alloc = c->alloc;
    if (alloc && alloc->free)
        alloc->free(alloc->ctx, c, sizeof(*c));
}

static const sc_observer_vtable_t log_vtable = {
    .record_event = log_record_event,
    .record_metric = log_record_metric,
    .flush = log_flush,
    .name = log_name,
    .deinit = log_deinit,
};

sc_observer_t sc_log_observer_create(sc_allocator_t *alloc, FILE *output) {
    if (!alloc) return (sc_observer_t){ .ctx = NULL, .vtable = NULL };

    sc_log_observer_ctx_t *ctx = (sc_log_observer_ctx_t *)alloc->alloc(alloc->ctx, sizeof(sc_log_observer_ctx_t));
    if (!ctx) return (sc_observer_t){ .ctx = NULL, .vtable = NULL };

    memset(ctx, 0, sizeof(*ctx));
    ctx->output = output;
    ctx->alloc = alloc;

    return (sc_observer_t){ .ctx = ctx, .vtable = &log_vtable };
}
