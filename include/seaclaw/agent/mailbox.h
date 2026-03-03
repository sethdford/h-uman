#ifndef SC_AGENT_MAILBOX_H
#define SC_AGENT_MAILBOX_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum sc_msg_type {
    SC_MSG_TASK,
    SC_MSG_RESULT,
    SC_MSG_ERROR,
    SC_MSG_CANCEL,
    SC_MSG_PING,
    SC_MSG_PONG,
} sc_msg_type_t;

typedef struct sc_message {
    sc_msg_type_t type;
    uint64_t from_agent;
    uint64_t to_agent;
    char *payload;
    size_t payload_len;
    uint64_t timestamp;
    uint64_t correlation_id;
} sc_message_t;

typedef struct sc_mailbox sc_mailbox_t;

sc_mailbox_t *sc_mailbox_create(sc_allocator_t *alloc, uint32_t max_agents);
void sc_mailbox_destroy(sc_mailbox_t *mbox);

sc_error_t sc_mailbox_register(sc_mailbox_t *mbox, uint64_t agent_id);
sc_error_t sc_mailbox_unregister(sc_mailbox_t *mbox, uint64_t agent_id);

sc_error_t sc_mailbox_send(sc_mailbox_t *mbox, uint64_t from_agent, uint64_t to_agent,
                           sc_msg_type_t type, const char *payload, size_t payload_len,
                           uint64_t correlation_id);

sc_error_t sc_mailbox_recv(sc_mailbox_t *mbox, uint64_t agent_id, sc_message_t *out);

sc_error_t sc_mailbox_broadcast(sc_mailbox_t *mbox, uint64_t from_agent, sc_msg_type_t type,
                                const char *payload, size_t payload_len);

size_t sc_mailbox_pending_count(sc_mailbox_t *mbox, uint64_t agent_id);

void sc_message_free(sc_allocator_t *alloc, sc_message_t *msg);

#endif /* SC_AGENT_MAILBOX_H */
