/*
 * PWA Channel — polls browser tabs for new messages and integrates into the agent loop.
 * Makes PWA automation bidirectional: the agent receives messages from web apps.
 */
#include "human/channels/pwa.h"
#include "human/channel_loop.h"
#include "human/core/string.h"
#include "human/pwa.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_PWA_SENT_RING_SIZE  32
#define HU_PWA_SENT_PREFIX_LEN 256
#define HU_PWA_MAX_APPS        16

typedef struct hu_pwa_channel_ctx {
    hu_allocator_t *alloc;
    hu_pwa_browser_t browser;
    bool browser_detected;
    bool running;
    char **app_names;
    size_t app_count;
    size_t app_cap; /* allocation capacity for app_names and last_content_hash */
    uint32_t *last_content_hash;
    char sent_ring[HU_PWA_SENT_RING_SIZE][HU_PWA_SENT_PREFIX_LEN];
    size_t sent_ring_len[HU_PWA_SENT_RING_SIZE];
    uint32_t sent_ring_hash[HU_PWA_SENT_RING_SIZE];
    size_t sent_ring_idx;
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char app[64];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} hu_pwa_channel_ctx_t;

#if !HU_IS_TEST
/* FNV-1a hash (same pattern as iMessage) */
static uint32_t pwa_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static void pwa_record_sent(hu_pwa_channel_ctx_t *c, const char *msg, size_t msg_len) {
    size_t slot = c->sent_ring_idx % HU_PWA_SENT_RING_SIZE;
    size_t copy_len =
        msg_len < HU_PWA_SENT_PREFIX_LEN - 1 ? msg_len : HU_PWA_SENT_PREFIX_LEN - 1;
    memcpy(c->sent_ring[slot], msg, copy_len);
    c->sent_ring[slot][copy_len] = '\0';
    c->sent_ring_len[slot] = copy_len;
    c->sent_ring_hash[slot] = pwa_hash(msg, msg_len);
    c->sent_ring_idx++;
}

static bool pwa_was_sent_by_us(hu_pwa_channel_ctx_t *c, const char *text, size_t text_len) {
    uint32_t h = pwa_hash(text, text_len);
    for (size_t i = 0; i < HU_PWA_SENT_RING_SIZE; i++) {
        size_t slen = c->sent_ring_len[i];
        if (slen == 0)
            continue;
        if (c->sent_ring_hash[i] == h) {
            size_t cmp_len = text_len < slen ? text_len : slen;
            if (cmp_len > 0 && memcmp(text, c->sent_ring[i], cmp_len) == 0)
                return true;
        }
    }
    return false;
}

/* Extract the last line from read_messages output (newest message). */
static const char *pwa_last_line(const char *content, size_t content_len, size_t *out_len) {
    if (!content || content_len == 0) {
        *out_len = 0;
        return NULL;
    }
    const char *last = content + content_len - 1;
    while (last > content && *last != '\n')
        last--;
    if (*last == '\n')
        last++;
    *out_len = (size_t)(content + content_len - last);
    return last;
}
#endif /* !HU_IS_TEST */

static hu_error_t pwa_start(void *ctx) {
    hu_pwa_channel_ctx_t *c = (hu_pwa_channel_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
#if HU_IS_TEST
    return HU_OK;
#else
    hu_error_t err = hu_pwa_detect_browser(&c->browser);
    if (err != HU_OK)
        return err;
    c->browser_detected = true;
    /* Validate at least one monitored tab is open */
    for (size_t i = 0; i < c->app_count; i++) {
        const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(c->app_names[i]);
        if (!drv || !drv->read_messages_js)
            continue;
        hu_pwa_tab_t tab;
        err = hu_pwa_find_tab(c->alloc, c->browser, drv->url_pattern, &tab);
        hu_pwa_tab_free(c->alloc, &tab);
        if (err == HU_OK)
            return HU_OK;
    }
    return HU_ERR_CHANNEL_NOT_CONFIGURED;
#endif
}

static void pwa_stop(void *ctx) {
    hu_pwa_channel_ctx_t *c = (hu_pwa_channel_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t pwa_send(void *ctx, const char *target, size_t target_len,
                           const char *message, size_t message_len, const char *const *media,
                           size_t media_count) {
    (void)media;
    (void)media_count;
    hu_pwa_channel_ctx_t *c = (hu_pwa_channel_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!message || message_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)target;
    (void)target_len;
    size_t len = message_len > 4095 ? 4095 : message_len;
    if (len > 0)
        memcpy(c->last_message, message, len);
    c->last_message[len] = '\0';
    c->last_message_len = len;
    return HU_OK;
#else
    if (!c->browser_detected) {
        hu_error_t err = hu_pwa_detect_browser(&c->browser);
        if (err != HU_OK)
            return err;
        c->browser_detected = true;
    }
    /* Target format: "pwa:slack" or "pwa:slack:channel". Extract app and optional channel. */
    const char *app = c->app_names[0];
    const char *tgt = "";
    if (target && target_len >= 5 && memcmp(target, "pwa:", 4) == 0) {
        const char *rest = target + 4;
        size_t rest_len = target_len - 4;
        const char *colon = memchr(rest, ':', rest_len);
        if (colon && (size_t)(colon - rest) < rest_len) {
            size_t app_len = (size_t)(colon - rest);
            char *app_buf = (char *)c->alloc->alloc(c->alloc->ctx, app_len + 1);
            if (!app_buf)
                return HU_ERR_OUT_OF_MEMORY;
            memcpy(app_buf, rest, app_len);
            app_buf[app_len] = '\0';
            app = app_buf;
            size_t tgt_len = rest_len - app_len - 1;
            char *tgt_buf = (char *)c->alloc->alloc(c->alloc->ctx, tgt_len + 1);
            if (!tgt_buf) {
                c->alloc->free(c->alloc->ctx, app_buf, app_len + 1);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(tgt_buf, colon + 1, tgt_len);
            tgt_buf[tgt_len] = '\0';
            tgt = tgt_buf;
            char *result = NULL;
            size_t result_len = 0;
            hu_error_t err = hu_pwa_send_message(c->alloc, c->browser, app, tgt, message, &result,
                                                 &result_len);
            c->alloc->free(c->alloc->ctx, app_buf, app_len + 1);
            c->alloc->free(c->alloc->ctx, tgt_buf, tgt_len + 1);
            if (result)
                c->alloc->free(c->alloc->ctx, result, result_len + 1);
            if (err == HU_OK)
                pwa_record_sent(c, message, message_len);
            return err;
        } else {
            char *app_buf = (char *)c->alloc->alloc(c->alloc->ctx, rest_len + 1);
            if (!app_buf)
                return HU_ERR_OUT_OF_MEMORY;
            memcpy(app_buf, rest, rest_len);
            app_buf[rest_len] = '\0';
            app = app_buf;
        }
    }
    char *result = NULL;
    size_t result_len = 0;
    hu_error_t err = hu_pwa_send_message(c->alloc, c->browser, app, tgt, message, &result,
                                         &result_len);
    if (app != c->app_names[0])
        c->alloc->free(c->alloc->ctx, (void *)app, strlen(app) + 1);
    if (result)
        c->alloc->free(c->alloc->ctx, result, result_len + 1);
    if (err == HU_OK)
        pwa_record_sent(c, message, message_len);
    return err;
#endif
}

static const char *pwa_name(void *ctx) {
    (void)ctx;
    return "pwa";
}

static bool pwa_health_check(void *ctx) {
    hu_pwa_channel_ctx_t *c = (hu_pwa_channel_ctx_t *)ctx;
#if HU_IS_TEST
    (void)c;
    return true;
#else
    if (!c)
        return false;
    hu_pwa_browser_t browser;
    if (hu_pwa_detect_browser(&browser) != HU_OK)
        return false;
    for (size_t i = 0; i < c->app_count; i++) {
        const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(c->app_names[i]);
        if (!drv || !drv->read_messages_js)
            continue;
        hu_pwa_tab_t tab;
        hu_error_t err = hu_pwa_find_tab(c->alloc, browser, drv->url_pattern, &tab);
        hu_pwa_tab_free(c->alloc, &tab);
        if (err == HU_OK)
            return true;
    }
    return false;
#endif
}

static const hu_channel_vtable_t pwa_vtable = {
    .start = pwa_start,
    .stop = pwa_stop,
    .send = pwa_send,
    .name = pwa_name,
    .health_check = pwa_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
    .load_conversation_history = NULL,
    .get_response_constraints = NULL,
    .react = NULL,
};

/* Build app list from all drivers with read_messages_js when apps is NULL. */
static const char *const PWA_APPS_WITH_READ[] = {
    "slack", "discord", "whatsapp", "gmail",     "calendar",
    "notion", "twitter", "telegram", "linkedin", "facebook",
};
#define PWA_APPS_WITH_READ_COUNT (sizeof(PWA_APPS_WITH_READ) / sizeof(PWA_APPS_WITH_READ[0]))

hu_error_t hu_pwa_channel_create(hu_allocator_t *alloc, const char *const *apps,
                                 size_t app_count, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_pwa_channel_ctx_t *c = (hu_pwa_channel_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;

    size_t count = 0;
    const char *const *src = NULL;

    if (apps && app_count > 0) {
        count = app_count > HU_PWA_MAX_APPS ? HU_PWA_MAX_APPS : app_count;
        src = apps;
    } else {
        count = PWA_APPS_WITH_READ_COUNT;
        src = PWA_APPS_WITH_READ;
    }

    c->app_names = (char **)alloc->alloc(alloc->ctx, count * sizeof(char *));
    if (!c->app_names) {
        alloc->free(alloc->ctx, c, sizeof(*c));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(c->app_names, 0, count * sizeof(char *));

    c->last_content_hash = (uint32_t *)alloc->alloc(alloc->ctx, count * sizeof(uint32_t));
    if (!c->last_content_hash) {
        alloc->free(alloc->ctx, c->app_names, count * sizeof(char *));
        alloc->free(alloc->ctx, c, sizeof(*c));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(c->last_content_hash, 0, count * sizeof(uint32_t));

    for (size_t i = 0; i < count; i++) {
        const char *name = src[i];
        if (!name)
            break;
        const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(name);
        if (!drv || !drv->read_messages_js)
            continue;
        c->app_names[c->app_count] = hu_strdup(alloc, name);
        if (!c->app_names[c->app_count]) {
            for (size_t j = 0; j < c->app_count; j++)
                alloc->free(alloc->ctx, c->app_names[j], strlen(c->app_names[j]) + 1);
            alloc->free(alloc->ctx, c->app_names, count * sizeof(char *));
            alloc->free(alloc->ctx, c->last_content_hash, count * sizeof(uint32_t));
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->app_count++;
    }

    if (c->app_count == 0) {
        alloc->free(alloc->ctx, c->app_names, count * sizeof(char *));
        alloc->free(alloc->ctx, c->last_content_hash, count * sizeof(uint32_t));
        alloc->free(alloc->ctx, c, sizeof(*c));
        return HU_ERR_INVALID_ARGUMENT;
    }

    c->app_cap = count;
    out->ctx = c;
    out->vtable = &pwa_vtable;
    return HU_OK;
}

void hu_pwa_channel_destroy(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return;
    hu_pwa_channel_ctx_t *c = (hu_pwa_channel_ctx_t *)ch->ctx;
    hu_allocator_t *a = c->alloc;
    for (size_t i = 0; i < c->app_count; i++) {
        if (c->app_names[i])
            a->free(a->ctx, c->app_names[i], strlen(c->app_names[i]) + 1);
    }
    if (c->app_names)
        a->free(a->ctx, c->app_names, c->app_cap * sizeof(char *));
    if (c->last_content_hash)
        a->free(a->ctx, c->last_content_hash, c->app_cap * sizeof(uint32_t));
    a->free(a->ctx, c, sizeof(*c));
    ch->ctx = NULL;
    ch->vtable = NULL;
}

hu_error_t hu_pwa_channel_poll(void *channel_ctx, hu_allocator_t *alloc,
                               hu_channel_loop_msg_t *msgs, size_t max_msgs, size_t *out_count) {
    (void)alloc;
    if (!channel_ctx || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    hu_pwa_channel_ctx_t *c = (hu_pwa_channel_ctx_t *)channel_ctx;

#if HU_IS_TEST
    if (c->mock_count > 0) {
        size_t n = c->mock_count < max_msgs ? c->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            int sn = snprintf(msgs[i].session_key, sizeof(msgs[i].session_key), "pwa:%s",
                             c->mock_msgs[i].app);
            if (sn <= 0 || (size_t)sn >= sizeof(msgs[i].session_key))
                msgs[i].session_key[0] = '\0';
            size_t ct = strlen(c->mock_msgs[i].content);
            if (ct >= sizeof(msgs[i].content))
                ct = sizeof(msgs[i].content) - 1;
            memcpy(msgs[i].content, c->mock_msgs[i].content, ct);
            msgs[i].content[ct] = '\0';
            msgs[i].is_group = false;
            msgs[i].message_id = -1;
            msgs[i].has_attachment = false;
            msgs[i].has_video = false;
            msgs[i].guid[0] = '\0';
        }
        *out_count = n;
        c->mock_count = 0;
        return HU_OK;
    }
    return HU_OK;
#else
    if (!c->running || !c->browser_detected)
        return HU_OK;

    for (size_t i = 0; i < c->app_count && *out_count < max_msgs; i++) {
        const char *app = c->app_names[i];
        char *raw = NULL;
        size_t raw_len = 0;
        hu_error_t err = hu_pwa_read_messages(c->alloc, c->browser, app, &raw, &raw_len);
        if (err != HU_OK || !raw)
            continue;

        uint32_t h = pwa_hash(raw, raw_len);
        if (h == c->last_content_hash[i]) {
            c->alloc->free(c->alloc->ctx, raw, raw_len + 1);
            continue;
        }

        size_t line_len = 0;
        const char *last_line = pwa_last_line(raw, raw_len, &line_len);
        if (!last_line || line_len == 0) {
            c->last_content_hash[i] = h;
            c->alloc->free(c->alloc->ctx, raw, raw_len + 1);
            continue;
        }

        /* Skip messages we sent (echo prevention) */
        if (pwa_was_sent_by_us(c, last_line, line_len)) {
            c->last_content_hash[i] = h;
            c->alloc->free(c->alloc->ctx, raw, raw_len + 1);
            continue;
        }

        int sn = snprintf(msgs[*out_count].session_key, sizeof(msgs[*out_count].session_key),
                         "pwa:%s", app);
        if (sn <= 0 || (size_t)sn >= sizeof(msgs[*out_count].session_key))
            msgs[*out_count].session_key[0] = '\0';

        size_t ct = line_len >= sizeof(msgs[*out_count].content)
                        ? sizeof(msgs[*out_count].content) - 1
                        : line_len;
        memcpy(msgs[*out_count].content, last_line, ct);
        msgs[*out_count].content[ct] = '\0';
        msgs[*out_count].is_group = false;
        msgs[*out_count].message_id = -1;
        msgs[*out_count].has_attachment = false;
        msgs[*out_count].has_video = false;
        msgs[*out_count].guid[0] = '\0';

        c->last_content_hash[i] = h;
        (*out_count)++;
        c->alloc->free(c->alloc->ctx, raw, raw_len + 1);
    }

    return HU_OK;
#endif
}

#if HU_IS_TEST
hu_error_t hu_pwa_channel_test_inject(hu_channel_t *ch, const char *app, const char *content) {
    if (!ch || !ch->ctx || !app || !content)
        return HU_ERR_INVALID_ARGUMENT;
    hu_pwa_channel_ctx_t *c = (hu_pwa_channel_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    size_t alen = strlen(app);
    if (alen >= sizeof(c->mock_msgs[i].app))
        alen = sizeof(c->mock_msgs[i].app) - 1;
    memcpy(c->mock_msgs[i].app, app, alen);
    c->mock_msgs[i].app[alen] = '\0';
    size_t clen = strlen(content);
    if (clen >= sizeof(c->mock_msgs[i].content))
        clen = sizeof(c->mock_msgs[i].content) - 1;
    memcpy(c->mock_msgs[i].content, content, clen);
    c->mock_msgs[i].content[clen] = '\0';
    return HU_OK;
}

const char *hu_pwa_channel_test_get_last(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_pwa_channel_ctx_t *c = (hu_pwa_channel_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
