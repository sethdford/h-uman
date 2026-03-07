#include "seaclaw/gateway/event_bridge.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#ifdef SC_HAS_PUSH
#include "seaclaw/gateway/push.h"
#endif
#include <stddef.h>
#include <string.h>

#ifdef SC_GATEWAY_POSIX

static bool bus_callback(sc_bus_event_type_t type, const sc_bus_event_t *ev, void *user_ctx) {
    sc_event_bridge_t *bridge = (sc_event_bridge_t *)user_ctx;
    if (!bridge || !bridge->proto || !bridge->proto->alloc)
        return true;

    const char *event_name = NULL;
    sc_json_value_t *payload_obj = sc_json_object_new(bridge->proto->alloc);
    if (!payload_obj)
        return true;

    switch (type) {
    case SC_BUS_MESSAGE_RECEIVED:
        event_name = "chat";
        sc_json_object_set(bridge->proto->alloc, payload_obj, "state",
                           sc_json_string_new(bridge->proto->alloc, "received", 8));
        break;
    case SC_BUS_MESSAGE_SENT:
        event_name = "chat";
        sc_json_object_set(bridge->proto->alloc, payload_obj, "state",
                           sc_json_string_new(bridge->proto->alloc, "sent", 4));
        break;
    case SC_BUS_MESSAGE_CHUNK:
        event_name = "chat";
        sc_json_object_set(bridge->proto->alloc, payload_obj, "state",
                           sc_json_string_new(bridge->proto->alloc, "chunk", 5));
        break;
    case SC_BUS_TOOL_CALL:
        event_name = "agent.tool";
        break;
    case SC_BUS_ERROR:
        event_name = "error";
        break;
    case SC_BUS_HEALTH_CHANGE:
        event_name = "health";
        break;
    case SC_BUS_CRON_STARTED:
        event_name = "cron.job";
        sc_json_object_set(bridge->proto->alloc, payload_obj, "state",
                           sc_json_string_new(bridge->proto->alloc, "started", 7));
        break;
    case SC_BUS_CRON_COMPLETED:
        event_name = "cron.job";
        sc_json_object_set(bridge->proto->alloc, payload_obj, "state",
                           sc_json_string_new(bridge->proto->alloc, "completed", 9));
        break;
    default:
        sc_json_free(bridge->proto->alloc, payload_obj);
        return true;
    }

    if (ev->channel[0]) {
        sc_json_object_set(
            bridge->proto->alloc, payload_obj, "channel",
            sc_json_string_new(bridge->proto->alloc, ev->channel, strlen(ev->channel)));
    }
    if (ev->id[0]) {
        sc_json_object_set(bridge->proto->alloc, payload_obj, "id",
                           sc_json_string_new(bridge->proto->alloc, ev->id, strlen(ev->id)));
    }
    const char *msg_text =
        (ev->payload) ? (const char *)ev->payload : (ev->message[0] ? ev->message : NULL);
    if (msg_text) {
        sc_json_object_set(bridge->proto->alloc, payload_obj, "message",
                           sc_json_string_new(bridge->proto->alloc, msg_text, strlen(msg_text)));
    }

    char *payload_str = NULL;
    size_t payload_len = 0;
    sc_error_t err =
        sc_json_stringify(bridge->proto->alloc, payload_obj, &payload_str, &payload_len);
    sc_json_free(bridge->proto->alloc, payload_obj);

    if (err == SC_OK && payload_str && event_name) {
        sc_control_send_event(bridge->proto, event_name, payload_str);
#ifdef SC_HAS_PUSH
        if (bridge->push &&
            (type == SC_BUS_MESSAGE_SENT || type == SC_BUS_ERROR ||
             type == SC_BUS_CRON_COMPLETED)) {
            const char *title;
            if (type == SC_BUS_CRON_COMPLETED) {
                title = (msg_text && strstr(msg_text, "failed")) ? "Automation Failed"
                                                                 : "Automation Completed";
            } else {
                title = (type == SC_BUS_MESSAGE_SENT) ? "New Message" : "Error";
            }
            const char *body = msg_text ? msg_text : event_name;
            sc_push_send(bridge->push, title, body, payload_str);
        }
#endif
        bridge->proto->alloc->free(bridge->proto->alloc->ctx, payload_str, payload_len + 1);
    }

    return true;
}

#endif /* SC_GATEWAY_POSIX */

void sc_event_bridge_init(sc_event_bridge_t *bridge, sc_control_protocol_t *proto, sc_bus_t *bus) {
    if (!bridge || !proto || !bus)
        return;

    bridge->proto = proto;
    bridge->bus = bus;
#ifdef SC_HAS_PUSH
    bridge->push = NULL;
#endif

#ifdef SC_GATEWAY_POSIX
    bridge->bus_cb = bus_callback;
    sc_bus_subscribe(bus, bus_callback, bridge, SC_BUS_EVENT_COUNT);
#endif
}

void sc_event_bridge_deinit(sc_event_bridge_t *bridge) {
    if (!bridge || !bridge->bus)
        return;

#ifdef SC_GATEWAY_POSIX
    sc_bus_unsubscribe(bridge->bus, bridge->bus_cb, bridge);
#endif
}

#ifdef SC_HAS_PUSH
void sc_event_bridge_set_push(sc_event_bridge_t *bridge, sc_push_manager_t *push) {
    if (bridge)
        bridge->push = push;
}
#endif
