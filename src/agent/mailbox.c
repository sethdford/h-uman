#include "seaclaw/agent/mailbox.h"
#include "seaclaw/core/string.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
#include <pthread.h>
#endif

#define SC_INBOX_CAP 256

typedef struct sc_inbox {
    uint64_t agent_id;
    sc_message_t msgs[SC_INBOX_CAP];
    uint32_t head;
    uint32_t tail;
    bool registered;
} sc_inbox_t;

struct sc_mailbox {
    sc_allocator_t *alloc;
    sc_inbox_t *inboxes;
    uint32_t max_agents;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_t mu;
#endif
};

static sc_inbox_t *find_inbox(sc_mailbox_t *m, uint64_t agent_id) {
    for (uint32_t i = 0; i < m->max_agents; i++)
        if (m->inboxes[i].registered && m->inboxes[i].agent_id == agent_id)
            return &m->inboxes[i];
    return NULL;
}

static uint32_t inbox_count(sc_inbox_t *ib) {
    return (ib->tail - ib->head + SC_INBOX_CAP) % SC_INBOX_CAP;
}

static bool inbox_full(sc_inbox_t *ib) {
    return ((ib->tail + 1) % SC_INBOX_CAP) == ib->head;
}

sc_mailbox_t *sc_mailbox_create(sc_allocator_t *alloc, uint32_t max_agents) {
    if (!alloc || max_agents == 0)
        return NULL;
    sc_mailbox_t *m = (sc_mailbox_t *)alloc->alloc(alloc->ctx, sizeof(*m));
    if (!m)
        return NULL;
    memset(m, 0, sizeof(*m));
    m->alloc = alloc;
    m->max_agents = max_agents;
    m->inboxes = (sc_inbox_t *)alloc->alloc(alloc->ctx, max_agents * sizeof(sc_inbox_t));
    if (!m->inboxes) {
        alloc->free(alloc->ctx, m, sizeof(*m));
        return NULL;
    }
    memset(m->inboxes, 0, max_agents * sizeof(sc_inbox_t));
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    if (pthread_mutex_init(&m->mu, NULL) != 0) {
        alloc->free(alloc->ctx, m->inboxes, max_agents * sizeof(sc_inbox_t));
        alloc->free(alloc->ctx, m, sizeof(*m));
        return NULL;
    }
#endif
    return m;
}

void sc_mailbox_destroy(sc_mailbox_t *mbox) {
    if (!mbox)
        return;
    for (uint32_t i = 0; i < mbox->max_agents; i++) {
        sc_inbox_t *ib = &mbox->inboxes[i];
        if (!ib->registered)
            continue;
        while (ib->head != ib->tail) {
            sc_message_t *msg = &ib->msgs[ib->head];
            if (msg->payload)
                mbox->alloc->free(mbox->alloc->ctx, msg->payload, msg->payload_len + 1);
            ib->head = (ib->head + 1) % SC_INBOX_CAP;
        }
    }
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pthread_mutex_destroy(&mbox->mu);
#endif
    mbox->alloc->free(mbox->alloc->ctx, mbox->inboxes, mbox->max_agents * sizeof(sc_inbox_t));
    mbox->alloc->free(mbox->alloc->ctx, mbox, sizeof(*mbox));
}

sc_error_t sc_mailbox_register(sc_mailbox_t *mbox, uint64_t agent_id) {
    if (!mbox)
        return SC_ERR_INVALID_ARGUMENT;
    if (find_inbox(mbox, agent_id))
        return SC_OK;
    for (uint32_t i = 0; i < mbox->max_agents; i++) {
        if (!mbox->inboxes[i].registered) {
            memset(&mbox->inboxes[i], 0, sizeof(sc_inbox_t));
            mbox->inboxes[i].agent_id = agent_id;
            mbox->inboxes[i].registered = true;
            return SC_OK;
        }
    }
    return SC_ERR_OUT_OF_MEMORY;
}

sc_error_t sc_mailbox_unregister(sc_mailbox_t *mbox, uint64_t agent_id) {
    if (!mbox)
        return SC_ERR_INVALID_ARGUMENT;
    sc_inbox_t *ib = find_inbox(mbox, agent_id);
    if (!ib)
        return SC_ERR_NOT_FOUND;
    while (ib->head != ib->tail) {
        sc_message_t *msg = &ib->msgs[ib->head];
        if (msg->payload)
            mbox->alloc->free(mbox->alloc->ctx, msg->payload, msg->payload_len + 1);
        ib->head = (ib->head + 1) % SC_INBOX_CAP;
    }
    ib->registered = false;
    return SC_OK;
}

sc_error_t sc_mailbox_send(sc_mailbox_t *mbox, uint64_t from_agent, uint64_t to_agent,
                           sc_msg_type_t type, const char *payload, size_t payload_len,
                           uint64_t correlation_id) {
    if (!mbox)
        return SC_ERR_INVALID_ARGUMENT;
    (void)from_agent;
    sc_inbox_t *ib = find_inbox(mbox, to_agent);
    if (!ib)
        return SC_ERR_NOT_FOUND;
    if (inbox_full(ib))
        return SC_ERR_OUT_OF_MEMORY;

    sc_message_t *msg = &ib->msgs[ib->tail];
    msg->type = type;
    msg->from_agent = from_agent;
    msg->to_agent = to_agent;
    msg->payload = payload ? sc_strndup(mbox->alloc, payload, payload_len) : NULL;
    msg->payload_len = payload_len;
    msg->timestamp = (uint64_t)time(NULL);
    msg->correlation_id = correlation_id;
    ib->tail = (ib->tail + 1) % SC_INBOX_CAP;
    return SC_OK;
}

sc_error_t sc_mailbox_recv(sc_mailbox_t *mbox, uint64_t agent_id, sc_message_t *out) {
    if (!mbox || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_inbox_t *ib = find_inbox(mbox, agent_id);
    if (!ib)
        return SC_ERR_NOT_FOUND;
    if (ib->head == ib->tail)
        return SC_ERR_NOT_FOUND;
    *out = ib->msgs[ib->head];
    ib->head = (ib->head + 1) % SC_INBOX_CAP;
    return SC_OK;
}

sc_error_t sc_mailbox_broadcast(sc_mailbox_t *mbox, uint64_t from_agent, sc_msg_type_t type,
                                const char *payload, size_t payload_len) {
    if (!mbox)
        return SC_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < mbox->max_agents; i++) {
        sc_inbox_t *ib = &mbox->inboxes[i];
        if (!ib->registered || ib->agent_id == from_agent)
            continue;
        sc_mailbox_send(mbox, from_agent, ib->agent_id, type, payload, payload_len, 0);
    }
    return SC_OK;
}

size_t sc_mailbox_pending_count(sc_mailbox_t *mbox, uint64_t agent_id) {
    if (!mbox)
        return 0;
    sc_inbox_t *ib = find_inbox(mbox, agent_id);
    if (!ib)
        return 0;
    return inbox_count(ib);
}

void sc_message_free(sc_allocator_t *alloc, sc_message_t *msg) {
    if (!alloc || !msg)
        return;
    if (msg->payload) {
        alloc->free(alloc->ctx, msg->payload, msg->payload_len + 1);
        msg->payload = NULL;
    }
}
