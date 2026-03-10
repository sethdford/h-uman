#ifndef HU_AGENT_MAILBOX_H
#define HU_AGENT_MAILBOX_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_msg_type {
    HU_MSG_TASK,
    HU_MSG_RESULT,
    HU_MSG_ERROR,
    HU_MSG_CANCEL,
    HU_MSG_PING,
    HU_MSG_PONG,
} hu_msg_type_t;

typedef struct hu_message {
    hu_msg_type_t type;
    uint64_t from_agent;
    uint64_t to_agent;
    char *payload;
    size_t payload_len;
    uint64_t timestamp;
    uint64_t correlation_id;
} hu_message_t;

typedef struct hu_mailbox hu_mailbox_t;

hu_mailbox_t *hu_mailbox_create(hu_allocator_t *alloc, uint32_t max_agents);
void hu_mailbox_destroy(hu_mailbox_t *mbox);

hu_error_t hu_mailbox_register(hu_mailbox_t *mbox, uint64_t agent_id);
hu_error_t hu_mailbox_unregister(hu_mailbox_t *mbox, uint64_t agent_id);

hu_error_t hu_mailbox_send(hu_mailbox_t *mbox, uint64_t from_agent, uint64_t to_agent,
                           hu_msg_type_t type, const char *payload, size_t payload_len,
                           uint64_t correlation_id);

hu_error_t hu_mailbox_recv(hu_mailbox_t *mbox, uint64_t agent_id, hu_message_t *out);

hu_error_t hu_mailbox_broadcast(hu_mailbox_t *mbox, uint64_t from_agent, hu_msg_type_t type,
                                const char *payload, size_t payload_len);

size_t hu_mailbox_pending_count(hu_mailbox_t *mbox, uint64_t agent_id);

void hu_message_free(hu_allocator_t *alloc, hu_message_t *msg);

#endif /* HU_AGENT_MAILBOX_H */
