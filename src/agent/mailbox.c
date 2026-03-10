#include "human/agent/mailbox.h"
#include "human/core/string.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
#include <pthread.h>
#endif

#define HU_INBOX_CAP 256

typedef struct hu_inbox {
    uint64_t agent_id;
    hu_message_t msgs[HU_INBOX_CAP];
    uint32_t head;
    uint32_t tail;
    bool registered;
} hu_inbox_t;

struct hu_mailbox {
    hu_allocator_t *alloc;
    hu_inbox_t *inboxes;
    uint32_t max_agents;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_t mu;
#endif
};

static hu_inbox_t *find_inbox(hu_mailbox_t *m, uint64_t agent_id) {
    for (uint32_t i = 0; i < m->max_agents; i++)
        if (m->inboxes[i].registered && m->inboxes[i].agent_id == agent_id)
            return &m->inboxes[i];
    return NULL;
}

static uint32_t inbox_count(hu_inbox_t *ib) {
    return (ib->tail - ib->head + HU_INBOX_CAP) % HU_INBOX_CAP;
}

static bool inbox_full(hu_inbox_t *ib) {
    return ((ib->tail + 1) % HU_INBOX_CAP) == ib->head;
}

hu_mailbox_t *hu_mailbox_create(hu_allocator_t *alloc, uint32_t max_agents) {
    if (!alloc || max_agents == 0)
        return NULL;
    hu_mailbox_t *m = (hu_mailbox_t *)alloc->alloc(alloc->ctx, sizeof(*m));
    if (!m)
        return NULL;
    memset(m, 0, sizeof(*m));
    m->alloc = alloc;
    m->max_agents = max_agents;
    m->inboxes = (hu_inbox_t *)alloc->alloc(alloc->ctx, max_agents * sizeof(hu_inbox_t));
    if (!m->inboxes) {
        alloc->free(alloc->ctx, m, sizeof(*m));
        return NULL;
    }
    memset(m->inboxes, 0, max_agents * sizeof(hu_inbox_t));
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    if (pthread_mutex_init(&m->mu, NULL) != 0) {
        alloc->free(alloc->ctx, m->inboxes, max_agents * sizeof(hu_inbox_t));
        alloc->free(alloc->ctx, m, sizeof(*m));
        return NULL;
    }
#endif
    return m;
}

void hu_mailbox_destroy(hu_mailbox_t *mbox) {
    if (!mbox)
        return;
    for (uint32_t i = 0; i < mbox->max_agents; i++) {
        hu_inbox_t *ib = &mbox->inboxes[i];
        if (!ib->registered)
            continue;
        while (ib->head != ib->tail) {
            hu_message_t *msg = &ib->msgs[ib->head];
            if (msg->payload)
                mbox->alloc->free(mbox->alloc->ctx, msg->payload, msg->payload_len + 1);
            ib->head = (ib->head + 1) % HU_INBOX_CAP;
        }
    }
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pthread_mutex_destroy(&mbox->mu);
#endif
    mbox->alloc->free(mbox->alloc->ctx, mbox->inboxes, mbox->max_agents * sizeof(hu_inbox_t));
    mbox->alloc->free(mbox->alloc->ctx, mbox, sizeof(*mbox));
}

hu_error_t hu_mailbox_register(hu_mailbox_t *mbox, uint64_t agent_id) {
    if (!mbox)
        return HU_ERR_INVALID_ARGUMENT;
    if (find_inbox(mbox, agent_id))
        return HU_OK;
    for (uint32_t i = 0; i < mbox->max_agents; i++) {
        if (!mbox->inboxes[i].registered) {
            memset(&mbox->inboxes[i], 0, sizeof(hu_inbox_t));
            mbox->inboxes[i].agent_id = agent_id;
            mbox->inboxes[i].registered = true;
            return HU_OK;
        }
    }
    return HU_ERR_OUT_OF_MEMORY;
}

hu_error_t hu_mailbox_unregister(hu_mailbox_t *mbox, uint64_t agent_id) {
    if (!mbox)
        return HU_ERR_INVALID_ARGUMENT;
    hu_inbox_t *ib = find_inbox(mbox, agent_id);
    if (!ib)
        return HU_ERR_NOT_FOUND;
    while (ib->head != ib->tail) {
        hu_message_t *msg = &ib->msgs[ib->head];
        if (msg->payload)
            mbox->alloc->free(mbox->alloc->ctx, msg->payload, msg->payload_len + 1);
        ib->head = (ib->head + 1) % HU_INBOX_CAP;
    }
    ib->registered = false;
    return HU_OK;
}

hu_error_t hu_mailbox_send(hu_mailbox_t *mbox, uint64_t from_agent, uint64_t to_agent,
                           hu_msg_type_t type, const char *payload, size_t payload_len,
                           uint64_t correlation_id) {
    if (!mbox)
        return HU_ERR_INVALID_ARGUMENT;
    (void)from_agent;
    hu_inbox_t *ib = find_inbox(mbox, to_agent);
    if (!ib)
        return HU_ERR_NOT_FOUND;
    if (inbox_full(ib))
        return HU_ERR_OUT_OF_MEMORY;

    hu_message_t *msg = &ib->msgs[ib->tail];
    msg->type = type;
    msg->from_agent = from_agent;
    msg->to_agent = to_agent;
    msg->payload = payload ? hu_strndup(mbox->alloc, payload, payload_len) : NULL;
    msg->payload_len = payload_len;
    msg->timestamp = (uint64_t)time(NULL);
    msg->correlation_id = correlation_id;
    ib->tail = (ib->tail + 1) % HU_INBOX_CAP;
    return HU_OK;
}

hu_error_t hu_mailbox_recv(hu_mailbox_t *mbox, uint64_t agent_id, hu_message_t *out) {
    if (!mbox || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_inbox_t *ib = find_inbox(mbox, agent_id);
    if (!ib)
        return HU_ERR_NOT_FOUND;
    if (ib->head == ib->tail)
        return HU_ERR_NOT_FOUND;
    *out = ib->msgs[ib->head];
    ib->head = (ib->head + 1) % HU_INBOX_CAP;
    return HU_OK;
}

hu_error_t hu_mailbox_broadcast(hu_mailbox_t *mbox, uint64_t from_agent, hu_msg_type_t type,
                                const char *payload, size_t payload_len) {
    if (!mbox)
        return HU_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < mbox->max_agents; i++) {
        hu_inbox_t *ib = &mbox->inboxes[i];
        if (!ib->registered || ib->agent_id == from_agent)
            continue;
        hu_mailbox_send(mbox, from_agent, ib->agent_id, type, payload, payload_len, 0);
    }
    return HU_OK;
}

size_t hu_mailbox_pending_count(hu_mailbox_t *mbox, uint64_t agent_id) {
    if (!mbox)
        return 0;
    hu_inbox_t *ib = find_inbox(mbox, agent_id);
    if (!ib)
        return 0;
    return inbox_count(ib);
}

void hu_message_free(hu_allocator_t *alloc, hu_message_t *msg) {
    if (!alloc || !msg)
        return;
    if (msg->payload) {
        alloc->free(alloc->ctx, msg->payload, msg->payload_len + 1);
        msg->payload = NULL;
    }
}
