#ifndef SC_EVENT_BRIDGE_H
#define SC_EVENT_BRIDGE_H

#include "seaclaw/gateway/control_protocol.h"
#include "seaclaw/bus.h"

typedef struct sc_event_bridge {
    sc_control_protocol_t *proto;
    sc_bus_t *bus;
    sc_bus_subscriber_fn bus_cb;
} sc_event_bridge_t;

void sc_event_bridge_init(sc_event_bridge_t *bridge,
    sc_control_protocol_t *proto, sc_bus_t *bus);
void sc_event_bridge_deinit(sc_event_bridge_t *bridge);

#endif /* SC_EVENT_BRIDGE_H */
