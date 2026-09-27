#ifndef HU_DAEMON_OUTBOUND_BUS_H
#define HU_DAEMON_OUTBOUND_BUS_H
/*
 * Outbound bus bridge (E2 carve-out of src/daemon.c, 2026-09-20).
 *
 * The service loop publishes agent stream events onto a process-local bus and
 * subscribes one bridge that turns them into channel deliveries: MESSAGE_CHUNK
 * → vtable->send_event (typing indicator on first chunk), MESSAGE_SENT →
 * vtable->send_event / send (rich embed for long Discord/Slack/Telegram text,
 * iMessage deferred to the post-turn action dispatcher so threading intent is
 * kept). Stream text chunks run the default outbound validator chain before
 * they reach the bus; a rejected chunk is dropped, an oversize one is clamped
 * to HU_BUS_MSG_LEN-1 on a UTF-8 boundary.
 *
 * Behaviour-preserving move: the bodies are unchanged from daemon.c; only the
 * names gained the hu_daemon_outbound_ prefix so tests can reach them.
 */
#include "human/agent.h"
#include "human/bus.h"
#include "human/core/allocator.h"
#include "human/daemon.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct hu_daemon_out_turn_state {
    bool typing_started;
    bool text_delivered_via_bus;
} hu_daemon_out_turn_state_t;

typedef struct hu_daemon_out_bus_bridge {
    hu_service_channel_t *channels;
    size_t channel_count;
    hu_daemon_out_turn_state_t *active_turn;   /* non-NULL during streaming (chunk typing) */
    hu_daemon_out_turn_state_t *delivery_turn; /* non-NULL around MESSAGE_SENT publish */
    hu_bus_t *bus;
} hu_daemon_out_bus_bridge_t;

typedef struct hu_daemon_stream_ctx {
    hu_bus_t *bus;
    char channel[HU_BUS_CHANNEL_LEN];
    char id[HU_BUS_ID_LEN];
    hu_allocator_t *alloc; /* optional; when non-NULL, stream text chunks run the outbound chain */
} hu_daemon_stream_ctx_t;

/* Longest prefix of buf[0..len) that does not end inside a multi-byte UTF-8
 * sequence. Returns len when the tail is already a complete character. */
size_t hu_daemon_outbound_utf8_safe_truncate(const char *buf, size_t len);

/* Copy data into bev->message, NUL-terminated, clamped to HU_BUS_MSG_LEN-1 on
 * a UTF-8 boundary. NULL/empty data clears the message. */
void hu_daemon_outbound_bus_set_message(hu_bus_event_t *bev, const char *data, size_t len);

/* Channel whose vtable->name equals `name`, or NULL. */
hu_service_channel_t *hu_daemon_outbound_find_channel(hu_service_channel_t *channels, size_t count,
                                                      const char *name);

/* hu_agent_stream_event_cb: ctx is a hu_daemon_stream_ctx_t. Publishes one bus
 * event per stream event (TEXT → MESSAGE_CHUNK after the outbound validator
 * chain when ctx->alloc is set; THINKING → THINKING_CHUNK; TOOL_* → TOOL_CALL /
 * TOOL_CALL_RESULT). */
void hu_daemon_outbound_stream_event_cb(const hu_agent_stream_event_t *event, void *ctx);

/* hu_bus_subscriber_fn: user_ctx is a hu_daemon_out_bus_bridge_t. Delivers
 * MESSAGE_CHUNK / MESSAGE_SENT to the named channel; always stays subscribed. */
bool hu_daemon_outbound_bus_cb(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *user_ctx);

#endif /* HU_DAEMON_OUTBOUND_BUS_H */
