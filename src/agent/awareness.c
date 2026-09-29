#include "human/agent/awareness.h"
#include "human/core/string.h"
#include "human/core/time.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t now_ms(void) {
    return (uint64_t)hu_time_wall_ms();
}

static void track_channel(hu_awareness_state_t *s, const char *channel) {
    if (!channel || channel[0] == '\0')
        return;
    for (size_t i = 0; i < s->active_channel_count; i++) {
        if (strncmp(s->active_channels[i], channel, HU_AWARENESS_CHANNEL_NAME_LEN - 1) == 0)
            return;
    }
    if (s->active_channel_count < HU_AWARENESS_MAX_ACTIVE_CHANNELS) {
        strncpy(s->active_channels[s->active_channel_count], channel,
                HU_AWARENESS_CHANNEL_NAME_LEN - 1);
        s->active_channels[s->active_channel_count][HU_AWARENESS_CHANNEL_NAME_LEN - 1] = '\0';
        s->active_channel_count++;
    }
}

static bool awareness_bus_cb(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *user_ctx) {
    hu_awareness_t *aw = (hu_awareness_t *)user_ctx;
    if (!aw)
        return false;
    hu_awareness_state_t *s = &aw->state;

    switch (type) {
    case HU_BUS_MESSAGE_RECEIVED:
        s->messages_received++;
        track_channel(s, ev->channel);
        break;
    case HU_BUS_MESSAGE_SENT:
        s->messages_sent++;
        track_channel(s, ev->channel);
        break;
    case HU_BUS_TOOL_CALL:
        s->tool_calls++;
        break;
    case HU_BUS_ERROR: {
        size_t idx = s->error_write_idx % HU_AWARENESS_MAX_RECENT_ERRORS;
        strncpy(s->recent_errors[idx].text, ev->message, HU_BUS_MSG_LEN - 1);
        s->recent_errors[idx].text[HU_BUS_MSG_LEN - 1] = '\0';
        s->recent_errors[idx].timestamp_ms = now_ms();
        s->error_write_idx++;
        s->total_errors++;
        break;
    }
    case HU_BUS_HEALTH_CHANGE:
        if (ev->message[0] != '\0')
            s->health_degraded = true;
        else
            s->health_degraded = false;
        break;
    default:
        break;
    }
    return true;
}

hu_error_t hu_awareness_init(hu_awareness_t *aw, hu_bus_t *bus) {
    if (!aw || !bus)
        return HU_ERR_INVALID_ARGUMENT;
    memset(&aw->state, 0, sizeof(aw->state));
    aw->bus = bus;
    return hu_bus_subscribe(bus, awareness_bus_cb, aw, HU_BUS_EVENT_COUNT);
}

void hu_awareness_deinit(hu_awareness_t *aw) {
    if (!aw || !aw->bus)
        return;
    hu_bus_unsubscribe(aw->bus, awareness_bus_cb, aw);
    aw->bus = NULL;
}

char *hu_awareness_context(const hu_awareness_t *aw, hu_allocator_t *alloc, size_t *out_len) {
    if (!aw || !alloc)
        return NULL;
    const hu_awareness_state_t *s = &aw->state;

    bool has_errors = s->total_errors > 0;
    bool has_stats = s->messages_received > 0 || s->tool_calls > 0;

    if (!has_errors && !has_stats && !s->health_degraded)
        return NULL;

    char buf[2048];
    size_t pos = 0;

    pos = hu_buf_appendf(buf, sizeof(buf), pos, "## Situational Awareness\n");
    if (pos >= sizeof(buf))
        pos = sizeof(buf) - 1;

    if (s->health_degraded) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "- WARNING: System health is degraded\n");
        if (pos >= sizeof(buf))
            pos = sizeof(buf) - 1;
    }

    if (has_stats) {
        pos =
            hu_buf_appendf(buf, sizeof(buf), pos,
                           "- Session stats: %llu msgs received, %llu sent, %llu tool calls\n",
                           (unsigned long long)s->messages_received,
                           (unsigned long long)s->messages_sent, (unsigned long long)s->tool_calls);
        if (pos >= sizeof(buf))
            pos = sizeof(buf) - 1;
    }

    if (s->active_channel_count > 0) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "- Active channels:");
        if (pos >= sizeof(buf))
            pos = sizeof(buf) - 1;
        for (size_t i = 0; i < s->active_channel_count && pos < sizeof(buf) - 40; i++) {
            pos = hu_buf_appendf(buf, sizeof(buf), pos, " %s", s->active_channels[i]);
            if (pos >= sizeof(buf))
                pos = sizeof(buf) - 1;
        }
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "\n");
        if (pos >= sizeof(buf))
            pos = sizeof(buf) - 1;
    }

    if (has_errors) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "- Recent errors (%llu total):\n",
                             (unsigned long long)s->total_errors);
        if (pos >= sizeof(buf))
            pos = sizeof(buf) - 1;
        size_t nerr = s->total_errors < HU_AWARENESS_MAX_RECENT_ERRORS
                          ? s->total_errors
                          : HU_AWARENESS_MAX_RECENT_ERRORS;
        for (size_t i = 0; i < nerr && pos < sizeof(buf) - 300; i++) {
            size_t idx = (s->error_write_idx + HU_AWARENESS_MAX_RECENT_ERRORS - nerr + i) %
                         HU_AWARENESS_MAX_RECENT_ERRORS;
            pos = hu_buf_appendf(buf, sizeof(buf), pos, "  - %s\n", s->recent_errors[idx].text);
            if (pos >= sizeof(buf))
                pos = sizeof(buf) - 1;
        }
    }
    if (pos >= sizeof(buf))
        pos = sizeof(buf) - 1;

    if (pos == 0)
        return NULL;

    char *result = (char *)alloc->alloc(alloc->ctx, pos + 1);
    if (!result)
        return NULL;
    memcpy(result, buf, pos);
    result[pos] = '\0';
    if (out_len)
        *out_len = pos;
    return result;
}
