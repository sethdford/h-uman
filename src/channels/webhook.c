/*
 * Webhook outbound channel — JSON callback formatting; inbound parsing for gateway.
 * HU_IS_TEST: send() is a no-op (no HTTP). Non-test: builds JSON only (curl optional).
 *
 * STATUS: ORPHAN (test-only). The gateway HTTP server uses webhook handlers
 * directly (see `src/gateway/gateway.c`); this generic channel facade is not
 * wired into bootstrap. Tracked in `docs/orphan-channels.md`.
 */
#include "human/channels/webhook.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <string.h>

#define HU_WEBHOOK_DEFAULT_MSG_FIELD "message"
#define HU_WEBHOOK_DEFAULT_SENDER_FIELD "sender"
#define HU_WEBHOOK_DEFAULT_MAX_MSG    4096
#define HU_WEBHOOK_OUTBOUND_SENDER    "human"

typedef struct hu_webhook_ctx {
    hu_allocator_t *alloc;
    char *name;
    char *callback_url;
    char *secret;
    char *message_field;
    char *sender_field;
    uint16_t max_message_len;
    bool running;
} hu_webhook_ctx_t;

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static const char *webhook_effective_msg_field(const hu_webhook_ctx_t *c) {
    return (c->message_field && c->message_field[0]) ? c->message_field : HU_WEBHOOK_DEFAULT_MSG_FIELD;
}
#endif

static hu_error_t webhook_start(void *ctx) {
    hu_webhook_ctx_t *c = (hu_webhook_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void webhook_stop(void *ctx) {
    hu_webhook_ctx_t *c = (hu_webhook_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t webhook_send(void *ctx, const char *target, size_t target_len, const char *message,
                               size_t message_len, const char *const *media, size_t media_count) {
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
    hu_webhook_ctx_t *c = (hu_webhook_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)message;
    (void)message_len;
    return HU_OK;
#else
    if (!c->callback_url || c->callback_url[0] == '\0')
        return HU_ERR_NOT_SUPPORTED;
    if (!c->running)
        return HU_ERR_INVALID_ARGUMENT;

    size_t cap = c->max_message_len > 0 ? (size_t)c->max_message_len : (size_t)HU_WEBHOOK_DEFAULT_MAX_MSG;
    size_t msg_use = message_len;
    if (msg_use > cap)
        msg_use = cap;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;
    err = hu_json_buf_append_raw(&jbuf, "{", 1);
    if (err)
        goto fail;
    {
        const char *mk = webhook_effective_msg_field(c);
        err = hu_json_append_key_value(&jbuf, mk, strlen(mk), message ? message : "", msg_use);
    }
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, ",", 1);
    if (err)
        goto fail;
    err = hu_json_append_key_value(&jbuf, "sender", 6, HU_WEBHOOK_OUTBOUND_SENDER,
                                   strlen(HU_WEBHOOK_OUTBOUND_SENDER));
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto fail;
    /* Payload built; real POST would use libcurl when HU_ENABLE_CURL. */
    hu_json_buf_free(&jbuf);
    return HU_OK;
fail:
    hu_json_buf_free(&jbuf);
    return err;
#endif
}

static const char *webhook_name(void *ctx) {
    hu_webhook_ctx_t *c = (hu_webhook_ctx_t *)ctx;
    if (c && c->name && c->name[0])
        return c->name;
    return "webhook";
}

static bool webhook_health_check(void *ctx) {
    hu_webhook_ctx_t *c = (hu_webhook_ctx_t *)ctx;
    return c != NULL;
}

static const hu_channel_vtable_t webhook_vtable = {
    .start = webhook_start,
    .stop = webhook_stop,
    .send = webhook_send,
    .name = webhook_name,
    .health_check = webhook_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
    .load_conversation_history = NULL,
    .get_response_constraints = NULL,
    .react = NULL,
    .reply = NULL,
    .react_emoji = NULL,
    .send_sticker = NULL,
    .get_attachment_path = NULL,
    .human_active_recently = NULL,
    .get_latest_attachment_path = NULL,
    .build_reaction_context = NULL,
    .build_read_receipt_context = NULL,
};

static void webhook_free_inner(hu_webhook_ctx_t *c, hu_allocator_t *alloc) {
    if (!c || !alloc)
        return;
    if (c->name) {
        size_t n = strlen(c->name) + 1;
        alloc->free(alloc->ctx, c->name, n);
    }
    if (c->callback_url) {
        size_t n = strlen(c->callback_url) + 1;
        alloc->free(alloc->ctx, c->callback_url, n);
    }
    if (c->secret) {
        size_t n = strlen(c->secret) + 1;
        alloc->free(alloc->ctx, c->secret, n);
    }
    if (c->message_field) {
        size_t n = strlen(c->message_field) + 1;
        alloc->free(alloc->ctx, c->message_field, n);
    }
    if (c->sender_field) {
        size_t n = strlen(c->sender_field) + 1;
        alloc->free(alloc->ctx, c->sender_field, n);
    }
}

hu_error_t hu_webhook_channel_create(hu_allocator_t *alloc,
                                     const hu_webhook_channel_config_t *cfg,
                                     hu_channel_t *out) {
    if (!alloc || !cfg || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_webhook_ctx_t *c = (hu_webhook_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->max_message_len = cfg->max_message_len;

    if (cfg->name && cfg->name[0]) {
        c->name = hu_strdup(alloc, cfg->name);
        if (!c->name)
            goto oom;
    }
    if (cfg->callback_url && cfg->callback_url[0]) {
        c->callback_url = hu_strdup(alloc, cfg->callback_url);
        if (!c->callback_url)
            goto oom;
    }
    if (cfg->secret && cfg->secret[0]) {
        c->secret = hu_strdup(alloc, cfg->secret);
        if (!c->secret)
            goto oom;
    }
    if (cfg->message_field && cfg->message_field[0]) {
        c->message_field = hu_strdup(alloc, cfg->message_field);
        if (!c->message_field)
            goto oom;
    }
    if (cfg->sender_field && cfg->sender_field[0]) {
        c->sender_field = hu_strdup(alloc, cfg->sender_field);
        if (!c->sender_field)
            goto oom;
    }

    out->ctx = c;
    out->vtable = &webhook_vtable;
    return HU_OK;
oom:
    webhook_free_inner(c, alloc);
    alloc->free(alloc->ctx, c, sizeof(*c));
    return HU_ERR_OUT_OF_MEMORY;
}

void hu_webhook_channel_destroy(hu_channel_t *ch, hu_allocator_t *alloc) {
    if (!ch || !ch->ctx)
        return;
    hu_webhook_ctx_t *c = (hu_webhook_ctx_t *)ch->ctx;
    hu_allocator_t *a = alloc ? alloc : c->alloc;
    if (!a)
        return;
    webhook_free_inner(c, a);
    a->free(a->ctx, c, sizeof(*c));
    ch->ctx = NULL;
    ch->vtable = NULL;
}

static void copy_json_string_field(const char *src, char *dst, size_t cap) {
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

hu_error_t hu_webhook_on_message(const char *body, size_t body_len,
                                 const hu_webhook_channel_config_t *cfg,
                                 char *sender_out, size_t sender_cap,
                                 char *message_out, size_t message_cap) {
    if (!cfg || !sender_out || sender_cap == 0 || !message_out || message_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    sender_out[0] = '\0';
    message_out[0] = '\0';
    if (!body && body_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *msg_key =
        (cfg->message_field && cfg->message_field[0]) ? cfg->message_field : HU_WEBHOOK_DEFAULT_MSG_FIELD;
    const char *snd_key =
        (cfg->sender_field && cfg->sender_field[0]) ? cfg->sender_field : HU_WEBHOOK_DEFAULT_SENDER_FIELD;

    hu_allocator_t pa = hu_system_allocator();
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(&pa, body ? body : "", body_len, &root);
    if (err != HU_OK || !root) {
        if (root)
            hu_json_free(&pa, root);
        return err != HU_OK ? err : HU_ERR_INVALID_ARGUMENT;
    }
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(&pa, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *msg = hu_json_get_string(root, msg_key);
    const char *snd = hu_json_get_string(root, snd_key);
    copy_json_string_field(snd, sender_out, sender_cap);
    copy_json_string_field(msg, message_out, message_cap);

    hu_json_free(&pa, root);
    return HU_OK;
}
