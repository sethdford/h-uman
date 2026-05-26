#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MATRIX_SESSION_KEY_MAX 127
#define MATRIX_CONTENT_MAX     4095

typedef struct hu_matrix_ctx {
    hu_allocator_t *alloc;
    char *homeserver;
    size_t homeserver_len;
    char *access_token;
    size_t access_token_len;
    bool running;
    char *since_token;
    char *user_id;
    time_t last_own_activity_unix;
    char last_own_room_id[256];
    size_t last_own_room_id_len;
    char mxc_server[256];
    char mxc_media[512];
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} hu_matrix_ctx_t;

#if defined(HU_HTTP_CURL)
static bool matrix_parse_mxc_url(const char *url, char *srv, size_t srv_cap, char *med,
                                 size_t med_cap) {
    if (!url || strncmp(url, "mxc://", 6) != 0 || srv_cap < 2 || med_cap < 2)
        return false;
    const char *p = url + 6;
    const char *slash = strchr(p, '/');
    if (!slash || slash == p)
        return false;
    size_t sl = (size_t)(slash - p);
    size_t ml = strlen(slash + 1);
    if (sl >= srv_cap || ml >= med_cap)
        return false;
    memcpy(srv, p, sl);
    srv[sl] = '\0';
    memcpy(med, slash + 1, ml);
    med[ml] = '\0';
    return true;
}

static int matrix_pct_encode_seg(const char *in, char *out, size_t out_cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 4 < out_cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '.' || c == '_') {
            out[j++] = (char)c;
        } else {
            int w = snprintf(out + j, out_cap - j, "%%%02X", (unsigned)c);
            if (w < 0 || (size_t)w >= out_cap - j)
                return -1;
            j += (size_t)w;
        }
    }
    if (j >= out_cap)
        return -1;
    out[j] = '\0';
    return 0;
}
#endif /* HU_HTTP_CURL */

static hu_error_t matrix_start(void *ctx) {
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void matrix_stop(void *ctx) {
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t matrix_send(void *ctx, const char *target, size_t target_len, const char *message,
                              size_t message_len, const char *const *media, size_t media_count) {
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;

#if HU_IS_TEST
    {
        size_t len = message_len > 4095 ? 4095 : message_len;
        if (message && len > 0)
            memcpy(c->last_message, message, len);
        c->last_message[len] = '\0';
        c->last_message_len = len;
        return HU_OK;
    }
#else
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->homeserver || c->homeserver_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!c->access_token || c->access_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;

    unsigned long txn_id = (unsigned long)time(NULL);
    char url_buf[1024];
    int n = snprintf(url_buf, sizeof(url_buf),
                     "%.*s/_matrix/client/r0/rooms/%.*s/send/m.room.message/%lu",
                     (int)c->homeserver_len, c->homeserver, (int)target_len, target, txn_id);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = hu_json_buf_append_raw(&jbuf, "{\"msgtype\":\"m.text\",", 20);
    if (err)
        goto jfail;
    err = hu_json_append_key_value(&jbuf, "body", 4, message, message_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto jfail;

    char headers_buf[600];
    n = snprintf(headers_buf, sizeof(headers_buf),
                 "Authorization: Bearer %.*s\nContent-Type: application/json",
                 (int)c->access_token_len, c->access_token);
    if (n <= 0 || (size_t)n >= sizeof(headers_buf)) {
        hu_json_buf_free(&jbuf);
        return HU_ERR_INTERNAL;
    }

    hu_http_response_t resp = {0};
    err = hu_http_request(c->alloc, url_buf, "PUT", headers_buf, jbuf.ptr, jbuf.len, &resp);
    hu_json_buf_free(&jbuf);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(c->alloc, &resp);
        return HU_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    if (resp.status_code < 200 || resp.status_code >= 300)
        return HU_ERR_CHANNEL_SEND;
    {
        size_t rl = target_len < sizeof(c->last_own_room_id) - 1 ? target_len
                                                                 : sizeof(c->last_own_room_id) - 1;
        memcpy(c->last_own_room_id, target, rl);
        c->last_own_room_id[rl] = '\0';
        c->last_own_room_id_len = rl;
        c->last_own_activity_unix = time(NULL);
    }
    return HU_OK;
jfail:
    hu_json_buf_free(&jbuf);
    return err;
#endif
}

static bool matrix_human_active_recently(void *ctx, const char *contact, size_t contact_len,
                                         int window_sec) {
#if HU_IS_TEST
    (void)ctx;
    (void)contact;
    (void)contact_len;
    (void)window_sec;
    return false;
#else
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;
    if (!c || !contact || contact_len == 0 || window_sec <= 0)
        return false;
    if (c->last_own_room_id_len != contact_len)
        return false;
    if (memcmp(c->last_own_room_id, contact, contact_len) != 0)
        return false;
    if (c->last_own_activity_unix == 0)
        return false;
    time_t now = time(NULL);
    return (now - c->last_own_activity_unix) <= (time_t)window_sec;
#endif
}

static const char *matrix_name(void *ctx) {
    (void)ctx;
    return "matrix";
}
static bool matrix_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static bool matrix_split_room_event(const char *target, size_t target_len, const char **room_id,
                                    size_t *room_len, const char **event_id, size_t *event_len) {
    for (size_t i = 0; i < target_len; i++) {
        if (target[i] == '|') {
            *room_id = target;
            *room_len = i;
            *event_id = target + i + 1;
            *event_len = target_len - i - 1;
            return *room_len > 0 && *event_len > 0;
        }
    }
    return false;
}

static const char *matrix_reaction_key(hu_reaction_type_t reaction) {
    switch (reaction) {
    case HU_REACTION_HEART:
        return "heart";
    case HU_REACTION_THUMBS_UP:
        return "+1";
    case HU_REACTION_THUMBS_DOWN:
        return "-1";
    case HU_REACTION_HAHA:
        return "smile";
    case HU_REACTION_EMPHASIS:
        return "exclamation";
    case HU_REACTION_QUESTION:
        return "question";
    default:
        return NULL;
    }
}

#if defined(HU_HTTP_CURL) && !HU_IS_TEST
static hu_error_t matrix_require_user_id(hu_matrix_ctx_t *c) {
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (c->user_id)
        return HU_OK;
    if (!c->homeserver || c->homeserver_len == 0 || !c->access_token || c->access_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    char url_buf[768];
    int nu = snprintf(url_buf, sizeof(url_buf), "%.*s/_matrix/client/v3/account/whoami",
                      (int)c->homeserver_len, c->homeserver);
    if (nu < 0 || (size_t)nu >= sizeof(url_buf))
        return HU_ERR_INTERNAL;
    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->access_token_len,
                      c->access_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(c->alloc, url_buf, auth_buf, &resp);
    if (err != HU_OK || !resp.body || resp.status_code != 200) {
        if (resp.owned && resp.body)
            hu_http_response_free(c->alloc, &resp);
        return err != HU_OK ? err : HU_ERR_CHANNEL_SEND;
    }
    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(c->alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    if (err != HU_OK || !parsed)
        return HU_ERR_CHANNEL_SEND;
    const char *uid = hu_json_get_string(parsed, "user_id");
    if (!uid || uid[0] == '\0') {
        hu_json_free(c->alloc, parsed);
        return HU_ERR_CHANNEL_SEND;
    }
    c->user_id = hu_strndup(c->alloc, uid, strlen(uid));
    hu_json_free(c->alloc, parsed);
    if (!c->user_id)
        return HU_ERR_OUT_OF_MEMORY;
    return HU_OK;
}
#endif

static hu_error_t matrix_start_typing(void *ctx, const char *recipient, size_t recipient_len) {
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;
    if (!c || !recipient || recipient_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    {
        hu_error_t e = matrix_require_user_id(c);
        if (e != HU_OK)
            return e;
        char room_tmp[256];
        size_t rl = recipient_len < sizeof(room_tmp) - 1 ? recipient_len : sizeof(room_tmp) - 1;
        memcpy(room_tmp, recipient, rl);
        room_tmp[rl] = '\0';
        char enc_room[512];
        char enc_uid[512];
        if (matrix_pct_encode_seg(room_tmp, enc_room, sizeof(enc_room)) != 0 ||
            matrix_pct_encode_seg(c->user_id, enc_uid, sizeof(enc_uid)) != 0)
            return HU_ERR_INTERNAL;
        char url_buf[1536];
        int n = snprintf(url_buf, sizeof(url_buf), "%.*s/_matrix/client/v3/rooms/%s/typing/%s",
                         (int)c->homeserver_len, c->homeserver, enc_room, enc_uid);
        if (n < 0 || (size_t)n >= sizeof(url_buf))
            return HU_ERR_INTERNAL;
        char headers_buf[640];
        int nh = snprintf(headers_buf, sizeof(headers_buf),
                          "Authorization: Bearer %.*s\nContent-Type: application/json",
                          (int)c->access_token_len, c->access_token);
        if (nh <= 0 || (size_t)nh >= sizeof(headers_buf))
            return HU_ERR_INTERNAL;
        static const char body[] = "{\"typing\":true,\"timeout\":30000}";
        hu_http_response_t resp = {0};
        e = hu_http_request(c->alloc, url_buf, "PUT", headers_buf, body, sizeof(body) - 1, &resp);
        if (resp.owned && resp.body)
            hu_http_response_free(c->alloc, &resp);
        if (e != HU_OK)
            return HU_ERR_CHANNEL_SEND;
        if (resp.status_code < 200 || resp.status_code >= 300)
            return HU_ERR_CHANNEL_SEND;
        return HU_OK;
    }
#else
    (void)recipient_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static hu_error_t matrix_stop_typing(void *ctx, const char *recipient, size_t recipient_len) {
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;
    if (!c || !recipient || recipient_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    {
        hu_error_t e = matrix_require_user_id(c);
        if (e != HU_OK)
            return e;
        char room_tmp[256];
        size_t rl = recipient_len < sizeof(room_tmp) - 1 ? recipient_len : sizeof(room_tmp) - 1;
        memcpy(room_tmp, recipient, rl);
        room_tmp[rl] = '\0';
        char enc_room[512];
        char enc_uid[512];
        if (matrix_pct_encode_seg(room_tmp, enc_room, sizeof(enc_room)) != 0 ||
            matrix_pct_encode_seg(c->user_id, enc_uid, sizeof(enc_uid)) != 0)
            return HU_ERR_INTERNAL;
        char url_buf[1536];
        int n = snprintf(url_buf, sizeof(url_buf), "%.*s/_matrix/client/v3/rooms/%s/typing/%s",
                         (int)c->homeserver_len, c->homeserver, enc_room, enc_uid);
        if (n < 0 || (size_t)n >= sizeof(url_buf))
            return HU_ERR_INTERNAL;
        char headers_buf[640];
        int nh = snprintf(headers_buf, sizeof(headers_buf),
                          "Authorization: Bearer %.*s\nContent-Type: application/json",
                          (int)c->access_token_len, c->access_token);
        if (nh <= 0 || (size_t)nh >= sizeof(headers_buf))
            return HU_ERR_INTERNAL;
        static const char body[] = "{\"typing\":false}";
        hu_http_response_t resp = {0};
        e = hu_http_request(c->alloc, url_buf, "PUT", headers_buf, body, sizeof(body) - 1, &resp);
        if (resp.owned && resp.body)
            hu_http_response_free(c->alloc, &resp);
        if (e != HU_OK)
            return HU_ERR_CHANNEL_SEND;
        if (resp.status_code < 200 || resp.status_code >= 300)
            return HU_ERR_CHANNEL_SEND;
        return HU_OK;
    }
#else
    (void)recipient_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static hu_error_t matrix_react(void *ctx, const char *target, size_t target_len, int64_t message_id,
                               hu_reaction_type_t reaction) {
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;
    (void)message_id;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!target || target_len == 0 || reaction == HU_REACTION_NONE)
        return HU_ERR_INVALID_ARGUMENT;
    const char *rkey = matrix_reaction_key(reaction);
    if (!rkey)
        return HU_ERR_INVALID_ARGUMENT;
    const char *room_id = NULL;
    size_t room_len = 0;
    const char *event_id = NULL;
    size_t event_len = 0;
    if (!matrix_split_room_event(target, target_len, &room_id, &room_len, &event_id, &event_len))
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)room_id;
    (void)room_len;
    (void)event_id;
    (void)event_len;
    (void)rkey;
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    if (!c->homeserver || c->homeserver_len == 0 || !c->access_token || c->access_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    {
        hu_json_buf_t jbuf;
        hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
        if (err)
            return err;
        err = hu_json_buf_append_raw(
            &jbuf, "{\"m.relates_to\":{\"rel_type\":\"m.annotation\",\"event_id\":", 58);
        if (err)
            goto mreact_fail;
        err = hu_json_append_string(&jbuf, event_id, event_len);
        if (err)
            goto mreact_fail;
        err = hu_json_buf_append_raw(&jbuf, ",\"key\":", 7);
        if (err)
            goto mreact_fail;
        err = hu_json_append_string(&jbuf, rkey, strlen(rkey));
        if (err)
            goto mreact_fail;
        err = hu_json_buf_append_raw(&jbuf, "}}", 2);
        if (err)
            goto mreact_fail;
        char room_tmp[256];
        size_t rml = room_len < sizeof(room_tmp) - 1 ? room_len : sizeof(room_tmp) - 1;
        memcpy(room_tmp, room_id, rml);
        room_tmp[rml] = '\0';
        char enc_room[512];
        if (matrix_pct_encode_seg(room_tmp, enc_room, sizeof(enc_room)) != 0) {
            hu_json_buf_free(&jbuf);
            return HU_ERR_INTERNAL;
        }
        unsigned long txn_id = (unsigned long)time(NULL);
        char url_buf[2048];
        int n = snprintf(url_buf, sizeof(url_buf),
                         "%.*s/_matrix/client/v3/rooms/%s/send/m.reaction/%lu",
                         (int)c->homeserver_len, c->homeserver, enc_room, txn_id);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            hu_json_buf_free(&jbuf);
            return HU_ERR_INTERNAL;
        }
        char headers_buf[640];
        int nh = snprintf(headers_buf, sizeof(headers_buf),
                          "Authorization: Bearer %.*s\nContent-Type: application/json",
                          (int)c->access_token_len, c->access_token);
        if (nh <= 0 || (size_t)nh >= sizeof(headers_buf)) {
            hu_json_buf_free(&jbuf);
            return HU_ERR_INTERNAL;
        }
        hu_http_response_t resp = {0};
        err = hu_http_request(c->alloc, url_buf, "PUT", headers_buf, jbuf.ptr, jbuf.len, &resp);
        hu_json_buf_free(&jbuf);
        if (resp.owned && resp.body)
            hu_http_response_free(c->alloc, &resp);
        if (err != HU_OK)
            return HU_ERR_CHANNEL_SEND;
        if (resp.status_code < 200 || resp.status_code >= 300)
            return HU_ERR_CHANNEL_SEND;
        return HU_OK;
    mreact_fail:
        hu_json_buf_free(&jbuf);
        return err;
    }
#else
    (void)room_id;
    (void)room_len;
    (void)event_id;
    (void)event_len;
    (void)rkey;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

#if defined(HU_HTTP_CURL) && !HU_IS_TEST
static hu_error_t matrix_load_conversation_history(void *ctx, hu_allocator_t *alloc,
                                                   const char *contact_id, size_t contact_id_len,
                                                   size_t limit, hu_channel_history_entry_t **out,
                                                   size_t *out_count) {
    if (!ctx || !alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;
    if (!c->homeserver || c->homeserver_len == 0 || !c->access_token || c->access_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;

    if (limit > 50)
        limit = 50;

    char url_buf[1024];
    int nu = snprintf(
        url_buf, sizeof(url_buf), "%.*s/_matrix/client/v3/rooms/%.*s/messages?dir=b&limit=%zu",
        (int)c->homeserver_len, c->homeserver, (int)contact_id_len, contact_id, limit);
    if (nu < 0 || (size_t)nu >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->access_token_len,
                      c->access_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url_buf, auth_buf, &resp);
    if (err != HU_OK || !resp.body || resp.status_code != 200) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err != HU_OK ? err : HU_ERR_INTERNAL;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !parsed)
        return HU_OK;

    hu_json_value_t *chunk = hu_json_object_get(parsed, "chunk");
    if (!chunk || chunk->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    size_t arr_len = chunk->data.array.len;
    if (arr_len > limit)
        arr_len = limit;

    hu_channel_history_entry_t *entries =
        (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, arr_len * sizeof(*entries));
    if (!entries) {
        hu_json_free(alloc, parsed);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, arr_len * sizeof(*entries));
    size_t count = 0;

    for (size_t i = 0; i < chunk->data.array.len && count < arr_len; i++) {
        hu_json_value_t *ev = chunk->data.array.items[i];
        if (!ev || ev->type != HU_JSON_OBJECT)
            continue;
        const char *ev_type = hu_json_get_string(ev, "type");
        if (!ev_type || strcmp(ev_type, "m.room.message") != 0)
            continue;
        const char *sender = hu_json_get_string(ev, "sender");
        hu_json_value_t *content = hu_json_object_get(ev, "content");
        if (!content)
            continue;
        const char *body = hu_json_get_string(content, "body");
        if (!body || strlen(body) == 0)
            continue;

        entries[count].from_me = (sender && c->user_id && strcmp(sender, c->user_id) == 0);

        size_t text_len = strlen(body);
        if (text_len > 511)
            text_len = 511;
        memcpy(entries[count].text, body, text_len);
        entries[count].text[text_len] = '\0';

        double ts_ms = hu_json_get_number(ev, "origin_server_ts", 0.0);
        time_t sec = (time_t)((int64_t)ts_ms / 1000);
        struct tm *tm = gmtime(&sec);
        if (tm)
            strftime(entries[count].timestamp, sizeof(entries[count].timestamp),
                     "%Y-%m-%dT%H:%M:%SZ", tm);

        count++;
    }

    hu_json_free(alloc, parsed);

    /* API returns newest first (dir=b); reverse for chronological order */
    for (size_t i = 0; i < count / 2; i++) {
        hu_channel_history_entry_t tmp = entries[i];
        entries[i] = entries[count - 1 - i];
        entries[count - 1 - i] = tmp;
    }

    *out = entries;
    *out_count = count;
    return HU_OK;
}
#else
static hu_error_t matrix_load_conversation_history(void *ctx, hu_allocator_t *alloc,
                                                   const char *contact_id, size_t contact_id_len,
                                                   size_t limit, hu_channel_history_entry_t **out,
                                                   size_t *out_count) {
    (void)ctx;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)limit;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}
#endif

static char *matrix_get_attachment_path(void *ctx, hu_allocator_t *alloc, int64_t message_id) {
    (void)message_id;
#if HU_IS_TEST
    (void)ctx;
    if (!alloc)
        return NULL;
    static const char k[] = "/tmp/test-attachment.jpg";
    size_t ln = strlen(k);
    char *copy = (char *)alloc->alloc(alloc->ctx, ln + 1);
    if (!copy)
        return NULL;
    memcpy(copy, k, ln + 1);
    return copy;
#elif defined(HU_HTTP_CURL)
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;
    if (!c || !alloc || !c->homeserver || c->homeserver_len == 0 || !c->access_token ||
        c->access_token_len == 0 || !c->mxc_media[0])
        return NULL;
    char enc_srv[384];
    char enc_med[768];
    if (matrix_pct_encode_seg(c->mxc_server, enc_srv, sizeof(enc_srv)) != 0 ||
        matrix_pct_encode_seg(c->mxc_media, enc_med, sizeof(enc_med)) != 0)
        return NULL;
    char url_buf[2048];
    int nu =
        snprintf(url_buf, sizeof(url_buf), "%.*s/_matrix/media/v3/download/%s/%s?access_token=%.*s",
                 (int)c->homeserver_len, c->homeserver, enc_srv, enc_med, (int)c->access_token_len,
                 c->access_token);
    if (nu < 0 || (size_t)nu >= sizeof(url_buf))
        return NULL;
    char *copy = (char *)alloc->alloc(alloc->ctx, (size_t)nu + 1);
    if (!copy)
        return NULL;
    memcpy(copy, url_buf, (size_t)nu + 1);
    return copy;
#else
    (void)ctx;
    (void)alloc;
    return NULL;
#endif
}

static const hu_channel_vtable_t matrix_vtable = {
    .start = matrix_start,
    .stop = matrix_stop,
    .send = matrix_send,
    .name = matrix_name,
    .health_check = matrix_health_check,
    .send_event = NULL,
    .start_typing = matrix_start_typing,
    .stop_typing = matrix_stop_typing,
    .load_conversation_history = matrix_load_conversation_history,
    .get_attachment_path = matrix_get_attachment_path,
    .react = matrix_react,
    .reply = NULL,
    .react_emoji = NULL,
    .send_sticker = NULL,
    .human_active_recently = matrix_human_active_recently,
};

hu_error_t hu_matrix_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                          size_t max_msgs, size_t *out_count) {
    (void)alloc;
    hu_matrix_ctx_t *ctx = (hu_matrix_ctx_t *)channel_ctx;
    if (!ctx || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if HU_IS_TEST
    if (ctx->mock_count > 0) {
        size_t n = ctx->mock_count < max_msgs ? ctx->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, ctx->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, ctx->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        ctx->mock_count = 0;
        return HU_OK;
    }
    return HU_OK;
#else
#if defined(HU_HTTP_CURL)
    if (!ctx->homeserver || ctx->homeserver_len == 0)
        return HU_OK;
    if (!ctx->access_token || ctx->access_token_len == 0)
        return HU_OK;
    if (!ctx->running)
        return HU_OK;
    (void)matrix_require_user_id(ctx);
    char url_buf[1024];
    int nu;
    if (ctx->since_token)
        nu = snprintf(url_buf, sizeof(url_buf),
                      "%.*s/_matrix/client/v3/"
                      "sync?timeout=5000&since=%s&filter={\"room\":{\"timeline\":{\"limit\":10}}}",
                      (int)ctx->homeserver_len, ctx->homeserver, ctx->since_token);
    else
        nu = snprintf(
            url_buf, sizeof(url_buf),
            "%.*s/_matrix/client/v3/sync?timeout=0&filter={\"room\":{\"timeline\":{\"limit\":10}}}",
            (int)ctx->homeserver_len, ctx->homeserver);
    if (nu < 0 || (size_t)nu >= sizeof(url_buf))
        return HU_ERR_INTERNAL;
    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %.*s",
                      (int)ctx->access_token_len, ctx->access_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url_buf, auth_buf, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return HU_OK;
    }
    if (resp.status_code != 200 || !resp.body) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return HU_OK;
    }
    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !parsed)
        return HU_OK;
    const char *next_batch = hu_json_get_string(parsed, "next_batch");
    if (next_batch) {
        if (ctx->since_token)
            ctx->alloc->free(ctx->alloc->ctx, ctx->since_token, strlen(ctx->since_token) + 1);
        ctx->since_token = hu_strndup(ctx->alloc, next_batch, strlen(next_batch));
    }
    size_t cnt = 0;
    hu_json_value_t *rooms = hu_json_object_get(parsed, "rooms");
    if (rooms) {
        hu_json_value_t *join = hu_json_object_get(rooms, "join");
        if (join && join->type == HU_JSON_OBJECT && join->data.object.pairs) {
            for (size_t r = 0; r < join->data.object.len && cnt < max_msgs; r++) {
                const char *room_id = join->data.object.pairs[r].key;
                hu_json_value_t *room = join->data.object.pairs[r].value;
                if (!room || room->type != HU_JSON_OBJECT)
                    continue;
                hu_json_value_t *tl = hu_json_object_get(room, "timeline");
                if (!tl)
                    continue;
                hu_json_value_t *events = hu_json_object_get(tl, "events");
                if (!events || events->type != HU_JSON_ARRAY)
                    continue;
                for (size_t e = 0; e < events->data.array.len && cnt < max_msgs; e++) {
                    hu_json_value_t *ev = events->data.array.items[e];
                    if (!ev || ev->type != HU_JSON_OBJECT)
                        continue;
                    const char *ev_type = hu_json_get_string(ev, "type");
                    /* Phase 2 of docs/plans/2026-05-18-imessage-sota.md:
                     * m.reaction events live in matrix_reactions.c to
                     * avoid the hu_reaction_type_t (channel.h) ↔
                     * hu_reaction_kind_t (reaction_event.h) enum
                     * collision. Dispatch before the m.room.message
                     * filter below, which would otherwise silently
                     * drop the reaction. */
                    extern int hu_matrix_handle_reaction_sync_event(
                        const hu_json_value_t *event, const char *room_id, const char *bot_user_id);
                    if (hu_matrix_handle_reaction_sync_event(ev, room_id, ctx->user_id))
                        continue;
                    if (!ev_type || strcmp(ev_type, "m.room.message") != 0)
                        continue;
                    const char *sender = hu_json_get_string(ev, "sender");
                    if (sender && ctx->user_id && strcmp(sender, ctx->user_id) == 0)
                        continue;
                    hu_json_value_t *content = hu_json_object_get(ev, "content");
                    if (!content)
                        continue;
                    const char *msgtype = hu_json_get_string(content, "msgtype");
                    const char *body = hu_json_get_string(content, "body");
                    const char *mxc_url = hu_json_get_string(content, "url");
                    const char *use_body = NULL;
                    bool is_media = false;
                    if (msgtype && strcmp(msgtype, "m.text") == 0 && body && strlen(body) > 0) {
                        use_body = body;
                    } else if (msgtype &&
                               (strcmp(msgtype, "m.image") == 0 ||
                                strcmp(msgtype, "m.video") == 0 ||
                                strcmp(msgtype, "m.file") == 0) &&
                               mxc_url && strncmp(mxc_url, "mxc://", 6) == 0) {
                        if (matrix_parse_mxc_url(mxc_url, ctx->mxc_server, sizeof(ctx->mxc_server),
                                                 ctx->mxc_media, sizeof(ctx->mxc_media))) {
                            use_body = (body && strlen(body) > 0) ? body : "(attachment)";
                            is_media = true;
                        }
                    }
                    if (!use_body)
                        continue;
                    size_t sk_len = room_id ? strlen(room_id) : 0;
                    if (sk_len > MATRIX_SESSION_KEY_MAX)
                        sk_len = MATRIX_SESSION_KEY_MAX;
                    if (room_id)
                        memcpy(msgs[cnt].session_key, room_id, sk_len);
                    msgs[cnt].session_key[sk_len] = '\0';
                    size_t ct_len = strlen(use_body);
                    if (ct_len > MATRIX_CONTENT_MAX)
                        ct_len = MATRIX_CONTENT_MAX;
                    memcpy(msgs[cnt].content, use_body, ct_len);
                    msgs[cnt].content[ct_len] = '\0';
                    msgs[cnt].has_attachment = is_media;
                    msgs[cnt].message_id = is_media ? 1 : -1;
                    cnt++;
                }
            }
        }
    }
    hu_json_free(alloc, parsed);
    *out_count = cnt;
    return HU_OK;
#else
    (void)alloc;
    (void)max_msgs;
    return HU_ERR_NOT_SUPPORTED;
#endif
#endif
}

hu_error_t hu_matrix_create(hu_allocator_t *alloc, const char *homeserver, size_t homeserver_len,
                            const char *access_token, size_t access_token_len, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (homeserver && homeserver_len > 0) {
        c->homeserver = (char *)alloc->alloc(alloc->ctx, homeserver_len + 1);
        if (!c->homeserver) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->homeserver, homeserver, homeserver_len);
        c->homeserver[homeserver_len] = '\0';
        c->homeserver_len = homeserver_len;
    }
    if (access_token && access_token_len > 0) {
        c->access_token = (char *)alloc->alloc(alloc->ctx, access_token_len + 1);
        if (!c->access_token) {
            if (c->homeserver)
                alloc->free(alloc->ctx, c->homeserver, homeserver_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->access_token, access_token, access_token_len);
        c->access_token[access_token_len] = '\0';
        c->access_token_len = access_token_len;
    }
    out->ctx = c;
    out->vtable = &matrix_vtable;
    return HU_OK;
}

void hu_matrix_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ch->ctx;
        if (c->alloc) {
            if (c->homeserver)
                c->alloc->free(c->alloc->ctx, c->homeserver, c->homeserver_len + 1);
            if (c->access_token)
                c->alloc->free(c->alloc->ctx, c->access_token, c->access_token_len + 1);
            if (c->since_token)
                c->alloc->free(c->alloc->ctx, c->since_token, strlen(c->since_token) + 1);
            if (c->user_id)
                c->alloc->free(c->alloc->ctx, c->user_id, strlen(c->user_id) + 1);
            c->alloc->free(c->alloc->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_matrix_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                      size_t session_key_len, const char *content,
                                      size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';
    return HU_OK;
}
const char *hu_matrix_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
