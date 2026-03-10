#include "human/gateway/event_bridge.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#ifdef HU_HAS_PUSH
#include "human/gateway/push.h"
#endif
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_GATEWAY_POSIX

static bool bus_callback(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *user_ctx) {
    hu_event_bridge_t *bridge = (hu_event_bridge_t *)user_ctx;
    if (!bridge || !bridge->proto || !bridge->proto->alloc)
        return true;

    const char *event_name = NULL;
    hu_json_value_t *payload_obj = hu_json_object_new(bridge->proto->alloc);
    if (!payload_obj)
        return true;

    switch (type) {
    case HU_BUS_MESSAGE_RECEIVED:
        event_name = "chat";
        hu_json_object_set(bridge->proto->alloc, payload_obj, "state",
                           hu_json_string_new(bridge->proto->alloc, "received", 8));
        break;
    case HU_BUS_MESSAGE_SENT:
        event_name = "chat";
        hu_json_object_set(bridge->proto->alloc, payload_obj, "state",
                           hu_json_string_new(bridge->proto->alloc, "sent", 4));
        break;
    case HU_BUS_MESSAGE_CHUNK:
        event_name = "chat";
        hu_json_object_set(bridge->proto->alloc, payload_obj, "state",
                           hu_json_string_new(bridge->proto->alloc, "chunk", 5));
        break;
    case HU_BUS_TOOL_CALL:
        event_name = "agent.tool";
        break;
    case HU_BUS_ERROR:
        event_name = "error";
        break;
    case HU_BUS_HEALTH_CHANGE:
        event_name = "health";
        break;
    case HU_BUS_CRON_STARTED:
        event_name = "cron.job";
        hu_json_object_set(bridge->proto->alloc, payload_obj, "state",
                           hu_json_string_new(bridge->proto->alloc, "started", 7));
        break;
    case HU_BUS_CRON_COMPLETED:
        event_name = "cron.job";
        hu_json_object_set(bridge->proto->alloc, payload_obj, "state",
                           hu_json_string_new(bridge->proto->alloc, "completed", 9));
        break;
    default:
        hu_json_free(bridge->proto->alloc, payload_obj);
        return true;
    }

    if (ev->channel[0]) {
        hu_json_object_set(
            bridge->proto->alloc, payload_obj, "channel",
            hu_json_string_new(bridge->proto->alloc, ev->channel, strlen(ev->channel)));
    }
    if (ev->id[0]) {
        hu_json_object_set(bridge->proto->alloc, payload_obj, "id",
                           hu_json_string_new(bridge->proto->alloc, ev->id, strlen(ev->id)));
    }
    const char *msg_text =
        (ev->payload) ? (const char *)ev->payload : (ev->message[0] ? ev->message : NULL);
    if (msg_text) {
        hu_json_object_set(bridge->proto->alloc, payload_obj, "message",
                           hu_json_string_new(bridge->proto->alloc, msg_text, strlen(msg_text)));
    }

    char *payload_str = NULL;
    size_t payload_len = 0;
    hu_error_t err =
        hu_json_stringify(bridge->proto->alloc, payload_obj, &payload_str, &payload_len);
    hu_json_free(bridge->proto->alloc, payload_obj);

    if (err == HU_OK && payload_str && event_name) {
        hu_control_send_event(bridge->proto, event_name, payload_str);
#ifdef HU_HAS_PUSH
        if (bridge->push && (type == HU_BUS_MESSAGE_SENT || type == HU_BUS_ERROR ||
                             type == HU_BUS_CRON_COMPLETED)) {
            const char *title;
            if (type == HU_BUS_CRON_COMPLETED) {
                title = (msg_text && strstr(msg_text, "failed")) ? "Automation Failed"
                                                                 : "Automation Completed";
            } else {
                title = (type == HU_BUS_MESSAGE_SENT) ? "New Message" : "Error";
            }
            const char *body = msg_text ? msg_text : event_name;
            hu_error_t push_err = hu_push_send(bridge->push, title, body, payload_str);
            if (push_err != HU_OK)
                (void)fprintf(stderr, "[event_bridge] push send failed: %s\n",
                              hu_error_string(push_err));
        }
#endif
        bridge->proto->alloc->free(bridge->proto->alloc->ctx, payload_str, payload_len + 1);
    }

    return true;
}

#endif /* HU_GATEWAY_POSIX */

void hu_event_bridge_init(hu_event_bridge_t *bridge, hu_control_protocol_t *proto, hu_bus_t *bus) {
    if (!bridge || !proto || !bus)
        return;

    bridge->proto = proto;
    bridge->bus = bus;
#ifdef HU_HAS_PUSH
    bridge->push = NULL;
#endif

#ifdef HU_GATEWAY_POSIX
    bridge->bus_cb = bus_callback;
    hu_bus_subscribe(bus, bus_callback, bridge, HU_BUS_EVENT_COUNT);
#endif
}

void hu_event_bridge_deinit(hu_event_bridge_t *bridge) {
    if (!bridge || !bridge->bus)
        return;

#ifdef HU_GATEWAY_POSIX
    hu_bus_unsubscribe(bridge->bus, bridge->bus_cb, bridge);
#endif
}

#ifdef HU_HAS_PUSH
void hu_event_bridge_set_push(hu_event_bridge_t *bridge, hu_push_manager_t *push) {
    if (bridge)
        bridge->push = push;
}
#endif
