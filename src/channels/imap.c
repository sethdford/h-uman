/*
 * IMAP channel — poll inbox for email, send stores in outbox (v1).
 * Real IMAP/SMTP requires a library; v1 implements vtable + HU_IS_TEST mock path.
 */
#include "human/channels/imap.h"
#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <stdlib.h>
#include <string.h>

#define HU_IMAP_OUTBOX_MAX      32
#define HU_IMAP_MOCK_QUEUE_MAX  16
#define HU_IMAP_SESSION_KEY_MAX 127
#define HU_IMAP_CONTENT_MAX     4095

typedef struct hu_imap_outbox_entry {
    char *target;
    char *message;
} hu_imap_outbox_entry_t;

#if HU_IS_TEST
typedef struct hu_imap_mock_msg {
    char session_key[HU_IMAP_SESSION_KEY_MAX + 1];
    char content[HU_IMAP_CONTENT_MAX + 1];
} hu_imap_mock_msg_t;
#endif

typedef struct hu_imap_ctx {
    hu_allocator_t *alloc;
    char *imap_host;
    size_t imap_host_len;
    uint16_t imap_port;
    char *imap_username;
    size_t imap_username_len;
    char *imap_password;
    size_t imap_password_len;
    char *imap_folder;
    size_t imap_folder_len;
    bool imap_use_tls;
    bool running;
    hu_imap_outbox_entry_t outbox[HU_IMAP_OUTBOX_MAX];
    size_t outbox_count;
#if HU_IS_TEST
    hu_imap_mock_msg_t mock_queue[HU_IMAP_MOCK_QUEUE_MAX];
    size_t mock_head;
    size_t mock_tail;
    size_t mock_count;
#endif
} hu_imap_ctx_t;

static hu_error_t imap_start(void *ctx) {
    hu_imap_ctx_t *c = (hu_imap_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void imap_stop(void *ctx) {
    hu_imap_ctx_t *c = (hu_imap_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t imap_send(void *ctx, const char *target, size_t target_len, const char *message,
                            size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    hu_imap_ctx_t *c = (hu_imap_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!target || target_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;
    if (c->outbox_count >= HU_IMAP_OUTBOX_MAX)
        return HU_ERR_OUT_OF_MEMORY;

#if HU_IS_TEST
    /* v1: store in outbox (log/store), no real SMTP */
    char *t = hu_strndup(c->alloc, target, target_len);
    char *m = hu_strndup(c->alloc, message, message_len);
    if (!t || !m) {
        if (t)
            c->alloc->free(c->alloc->ctx, t, target_len + 1);
        if (m)
            c->alloc->free(c->alloc->ctx, m, message_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    c->outbox[c->outbox_count].target = t;
    c->outbox[c->outbox_count].message = m;
    c->outbox_count++;
    return HU_OK;
#else
    /* Non-test: would send via SMTP; for v1 just store in outbox */
    (void)target_len;
    (void)message_len;
    char *t = hu_strndup(c->alloc, target, target_len);
    char *m = hu_strndup(c->alloc, message, message_len);
    if (!t || !m) {
        if (t)
            c->alloc->free(c->alloc->ctx, t, target_len + 1);
        if (m)
            c->alloc->free(c->alloc->ctx, m, message_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    c->outbox[c->outbox_count].target = t;
    c->outbox[c->outbox_count].message = m;
    c->outbox_count++;
    return HU_OK;
#endif
}

static const char *imap_name(void *ctx) {
    (void)ctx;
    return "imap";
}

static bool imap_health_check(void *ctx) {
    hu_imap_ctx_t *c = (hu_imap_ctx_t *)ctx;
    if (!c)
        return false;
    return (c->imap_host && c->imap_host[0] && c->imap_username && c->imap_username[0] &&
            c->imap_password && c->imap_password[0]);
}

static const hu_channel_vtable_t imap_vtable = {
    .start = imap_start,
    .stop = imap_stop,
    .send = imap_send,
    .name = imap_name,
    .health_check = imap_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_imap_create(hu_allocator_t *alloc, const hu_imap_config_t *config,
                          hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_imap_ctx_t *c = (hu_imap_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->imap_port = config && config->imap_port > 0 ? config->imap_port : 993;
    c->imap_use_tls = config ? config->imap_use_tls : true;

    if (config && config->imap_host && config->imap_host_len > 0) {
        c->imap_host = (char *)alloc->alloc(alloc->ctx, config->imap_host_len + 1);
        if (!c->imap_host) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->imap_host, config->imap_host, config->imap_host_len);
        c->imap_host[config->imap_host_len] = '\0';
        c->imap_host_len = config->imap_host_len;
    }
    if (config && config->imap_username && config->imap_username_len > 0) {
        c->imap_username = (char *)alloc->alloc(alloc->ctx, config->imap_username_len + 1);
        if (!c->imap_username) {
            if (c->imap_host)
                alloc->free(alloc->ctx, c->imap_host, config->imap_host_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->imap_username, config->imap_username, config->imap_username_len);
        c->imap_username[config->imap_username_len] = '\0';
        c->imap_username_len = config->imap_username_len;
    }
    if (config && config->imap_password && config->imap_password_len > 0) {
        c->imap_password = (char *)alloc->alloc(alloc->ctx, config->imap_password_len + 1);
        if (!c->imap_password) {
            if (c->imap_host)
                alloc->free(alloc->ctx, c->imap_host, config->imap_host_len + 1);
            if (c->imap_username)
                alloc->free(alloc->ctx, c->imap_username, config->imap_username_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->imap_password, config->imap_password, config->imap_password_len);
        c->imap_password[config->imap_password_len] = '\0';
        c->imap_password_len = config->imap_password_len;
    }
    if (config && config->imap_folder && config->imap_folder_len > 0) {
        c->imap_folder = (char *)alloc->alloc(alloc->ctx, config->imap_folder_len + 1);
        if (!c->imap_folder) {
            if (c->imap_host)
                alloc->free(alloc->ctx, c->imap_host, config->imap_host_len + 1);
            if (c->imap_username)
                alloc->free(alloc->ctx, c->imap_username, config->imap_username_len + 1);
            if (c->imap_password)
                alloc->free(alloc->ctx, c->imap_password, config->imap_password_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->imap_folder, config->imap_folder, config->imap_folder_len);
        c->imap_folder[config->imap_folder_len] = '\0';
        c->imap_folder_len = config->imap_folder_len;
    } else {
        c->imap_folder = hu_strndup(c->alloc, "INBOX", 5);
        c->imap_folder_len = 5;
    }

    out->ctx = c;
    out->vtable = &imap_vtable;
    return HU_OK;
}

void hu_imap_destroy(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return;
    hu_imap_ctx_t *c = (hu_imap_ctx_t *)ch->ctx;
    if (c->alloc) {
        for (size_t i = 0; i < c->outbox_count; i++) {
            if (c->outbox[i].target)
                c->alloc->free(c->alloc->ctx, c->outbox[i].target, strlen(c->outbox[i].target) + 1);
            if (c->outbox[i].message)
                c->alloc->free(c->alloc->ctx, c->outbox[i].message,
                               strlen(c->outbox[i].message) + 1);
        }
        if (c->imap_host)
            c->alloc->free(c->alloc->ctx, c->imap_host, c->imap_host_len + 1);
        if (c->imap_username)
            c->alloc->free(c->alloc->ctx, c->imap_username, c->imap_username_len + 1);
        if (c->imap_password)
            c->alloc->free(c->alloc->ctx, c->imap_password, c->imap_password_len + 1);
        if (c->imap_folder)
            c->alloc->free(c->alloc->ctx, c->imap_folder, c->imap_folder_len + 1);
        c->alloc->free(c->alloc->ctx, c, sizeof(*c));
    }
    ch->ctx = NULL;
    ch->vtable = NULL;
}

bool hu_imap_is_configured(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    hu_imap_ctx_t *c = (hu_imap_ctx_t *)ch->ctx;
    return c->imap_host != NULL && c->imap_host[0] != '\0' && c->imap_username != NULL &&
           c->imap_username[0] != '\0' && c->imap_password != NULL && c->imap_password[0] != '\0';
}

hu_error_t hu_imap_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                        size_t max_msgs, size_t *out_count) {
    if (!channel_ctx || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if HU_IS_TEST
    (void)alloc;
    hu_imap_ctx_t *c = (hu_imap_ctx_t *)channel_ctx;
    if (c->mock_count == 0)
        return HU_OK;
    size_t n = 0;
    while (n < max_msgs && c->mock_count > 0) {
        size_t idx = c->mock_head;
        size_t sk_len = strlen(c->mock_queue[idx].session_key);
        size_t ct_len = strlen(c->mock_queue[idx].content);
        if (sk_len >= sizeof(msgs[n].session_key))
            sk_len = sizeof(msgs[n].session_key) - 1;
        if (ct_len >= sizeof(msgs[n].content))
            ct_len = sizeof(msgs[n].content) - 1;
        memcpy(msgs[n].session_key, c->mock_queue[idx].session_key, sk_len);
        msgs[n].session_key[sk_len] = '\0';
        memcpy(msgs[n].content, c->mock_queue[idx].content, ct_len);
        msgs[n].content[ct_len] = '\0';
        c->mock_head = (c->mock_head + 1) % HU_IMAP_MOCK_QUEUE_MAX;
        c->mock_count--;
        n++;
    }
    *out_count = n;
    return HU_OK;
#else
    (void)alloc;
    (void)channel_ctx;
    (void)msgs;
    (void)max_msgs;
    /* Real IMAP requires a library; v1 returns empty */
    return HU_OK;
#endif
}

#if HU_IS_TEST
hu_error_t hu_imap_test_push_mock(hu_channel_t *ch, const char *session_key, size_t session_key_len,
                                  const char *content, size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_imap_ctx_t *c = (hu_imap_ctx_t *)ch->ctx;
    if (c->mock_count >= HU_IMAP_MOCK_QUEUE_MAX)
        return HU_ERR_OUT_OF_MEMORY;
    size_t idx = c->mock_tail;
    size_t sk_copy =
        session_key_len > HU_IMAP_SESSION_KEY_MAX ? HU_IMAP_SESSION_KEY_MAX : session_key_len;
    size_t ct_copy = content_len > HU_IMAP_CONTENT_MAX ? HU_IMAP_CONTENT_MAX : content_len;
    memcpy(c->mock_queue[idx].session_key, session_key, sk_copy);
    c->mock_queue[idx].session_key[sk_copy] = '\0';
    memcpy(c->mock_queue[idx].content, content, ct_copy);
    c->mock_queue[idx].content[ct_copy] = '\0';
    c->mock_tail = (c->mock_tail + 1) % HU_IMAP_MOCK_QUEUE_MAX;
    c->mock_count++;
    return HU_OK;
}
#endif
