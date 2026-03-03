#ifndef SC_EVENT_BRIDGE_H
#define SC_EVENT_BRIDGE_H

#include "seaclaw/bus.h"
#include "seaclaw/gateway/control_protocol.h"
#include "seaclaw/gateway/push.h"

typedef struct sc_event_bridge {
    sc_control_protocol_t *proto;
    sc_bus_t *bus;
    sc_bus_subscriber_fn bus_cb;
    sc_push_manager_t *push; /* optional; if non-NULL, forward events as push notifications */
} sc_event_bridge_t;

void sc_event_bridge_init(sc_event_bridge_t *bridge, sc_control_protocol_t *proto, sc_bus_t *bus);
void sc_event_bridge_deinit(sc_event_bridge_t *bridge);

void sc_event_bridge_set_push(sc_event_bridge_t *bridge, sc_push_manager_t *push);

#endif /* SC_EVENT_BRIDGE_H */
