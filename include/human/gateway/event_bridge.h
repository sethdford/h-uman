#ifndef HU_EVENT_BRIDGE_H
#define HU_EVENT_BRIDGE_H

#include "human/bus.h"
#include "human/gateway/control_protocol.h"
#ifdef HU_HAS_PUSH
#include "human/gateway/push.h"
#endif

typedef struct hu_event_bridge {
    hu_control_protocol_t *proto;
    hu_bus_t *bus;
    hu_bus_subscriber_fn bus_cb;
#ifdef HU_HAS_PUSH
    hu_push_manager_t *push;
#endif
} hu_event_bridge_t;

void hu_event_bridge_init(hu_event_bridge_t *bridge, hu_control_protocol_t *proto, hu_bus_t *bus);
void hu_event_bridge_deinit(hu_event_bridge_t *bridge);

#ifdef HU_HAS_PUSH
void hu_event_bridge_set_push(hu_event_bridge_t *bridge, hu_push_manager_t *push);
#endif

#endif /* HU_EVENT_BRIDGE_H */
