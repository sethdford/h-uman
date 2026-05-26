#include "human/channels/teams.h"
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEAMS_QUEUE_MAX       32
#define TEAMS_SESSION_KEY_MAX 127
#define TEAMS_CONTENT_MAX     4095
#define TEAMS_GRAPH_API_BASE  "https://graph.microsoft.com/v1.0"

typedef struct hu_teams_queued_msg {
    char session_key[128];
    char content[4096];
    time_t received_at;
} hu_teams_queued_msg_t;

typedef struct hu_teams_ctx {
    hu_allocator_t *alloc;
    char *webhook_url;
    size_t webhook_url_len;
    char *graph_access_token;
    size_t graph_access_token_len;
    char *team_id;
    size_t team_id_len;
    bool running;
    hu_teams_queued_msg_t queue[TEAMS_QUEUE_MAX];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} hu_teams_ctx_t;

static hu_error_t teams_start(void *ctx) {
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void teams_stop(void *ctx) {
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static void teams_queue_push(hu_teams_ctx_t *c, const char *from, size_t from_len, const char *body,
                             size_t body_len) {
    if (c->queue_count >= TEAMS_QUEUE_MAX)
        return;
    hu_teams_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < TEAMS_SESSION_KEY_MAX ? from_len : TEAMS_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < TEAMS_CONTENT_MAX ? body_len : TEAMS_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    slot->received_at = time(NULL);
    c->queue_tail = (c->queue_tail + 1) % TEAMS_QUEUE_MAX;
    c->queue_count++;
}

static hu_error_t teams_send(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    (void)target;
    (void)target_len;
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)ctx;

#if HU_IS_TEST
    if (!c->webhook_url || c->webhook_url_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
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
    if (!c->webhook_url || c->webhook_url_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!message)
        return HU_ERR_INVALID_ARGUMENT;
    if (c->webhook_url_len < 9 || strncmp(c->webhook_url, "https://", 8) != 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;
    err = hu_json_buf_append_raw(&jbuf, "{\"text\":", 8);
    if (err)
        goto jfail;
    err = hu_json_append_string(&jbuf, message, message_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto jfail;

    hu_http_response_t resp = {0};
    err = hu_http_post_json(c->alloc, c->webhook_url, NULL, jbuf.ptr, jbuf.len, &resp);
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
    return HU_OK;
jfail:
    hu_json_buf_free(&jbuf);
    return err;
#endif
}

static const char *teams_name(void *ctx) {
    (void)ctx;
    return "teams";
}

static bool teams_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static hu_error_t teams_get_response_constraints(void *ctx, hu_channel_response_constraints_t *out) {
    (void)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->max_chars = 28000;
    return HU_OK;
}

/* React target: "chatId|graphMessageId". */
static bool teams_split_chat_message(const char *target, size_t target_len, const char **chat_id,
                                     size_t *chat_len, const char **msg_id, size_t *msg_len) {
    for (size_t i = 0; i < target_len; i++) {
        if (target[i] == '|') {
            *chat_id = target;
            *chat_len = i;
            *msg_id = target + i + 1;
            *msg_len = target_len - i - 1;
            return *chat_len > 0 && *msg_len > 0;
        }
    }
    return false;
}

static const char *teams_graph_reaction_type(hu_reaction_type_t reaction) {
    switch (reaction) {
    case HU_REACTION_HEART:
        return "heart";
    case HU_REACTION_THUMBS_UP:
        return "like";
    case HU_REACTION_THUMBS_DOWN:
        return "sad";
    case HU_REACTION_HAHA:
        return "laugh";
    case HU_REACTION_EMPHASIS:
        return "surprised";
    case HU_REACTION_QUESTION:
        return "surprised";
    default:
        return NULL;
    }
}

static hu_error_t teams_start_typing(void *ctx, const char *recipient, size_t recipient_len) {
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)ctx;
    if (!c || !recipient || recipient_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    if (!c->graph_access_token || c->graph_access_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    char url_buf[2048];
    int n = snprintf(url_buf, sizeof(url_buf), TEAMS_GRAPH_API_BASE "/chats/%.*s/sendTypingIndicator",
                     (int)recipient_len, recipient);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;
    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->graph_access_token_len,
                      c->graph_access_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;
    static const char body[] = "{\"isTyping\":true}";
    hu_http_response_t resp = {0};
    hu_error_t err =
        hu_http_post_json(c->alloc, url_buf, auth_buf, body, sizeof(body) - 1, &resp);
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    if (err != HU_OK)
        return HU_ERR_CHANNEL_SEND;
    if (resp.status_code < 200 || resp.status_code >= 300)
        return HU_ERR_CHANNEL_SEND;
    return HU_OK;
#else
    (void)recipient_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static hu_error_t teams_stop_typing(void *ctx, const char *recipient, size_t recipient_len) {
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)ctx;
    if (!c || !recipient || recipient_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    if (!c->graph_access_token || c->graph_access_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    char url_buf[2048];
    int n = snprintf(url_buf, sizeof(url_buf), TEAMS_GRAPH_API_BASE "/chats/%.*s/sendTypingIndicator",
                     (int)recipient_len, recipient);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;
    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->graph_access_token_len,
                      c->graph_access_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;
    static const char body[] = "{\"isTyping\":false}";
    hu_http_response_t resp = {0};
    hu_error_t err =
        hu_http_post_json(c->alloc, url_buf, auth_buf, body, sizeof(body) - 1, &resp);
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    if (err != HU_OK)
        return HU_ERR_CHANNEL_SEND;
    if (resp.status_code < 200 || resp.status_code >= 300)
        return HU_ERR_CHANNEL_SEND;
    return HU_OK;
#else
    (void)recipient_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

#if HU_IS_TEST
static hu_error_t teams_load_conversation_history(void *ctx, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t contact_id_len,
                                                  size_t limit, hu_channel_history_entry_t **out,
                                                  size_t *out_count) {
    (void)ctx;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)limit;
    if (!out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    return HU_OK;
}
#elif defined(HU_HTTP_CURL)
static size_t teams_pct_encode_path_seg(const char *in, size_t in_len, char *out, size_t out_cap) {
    size_t j = 0;
    for (size_t i = 0; i < in_len && j + 4 < out_cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out[j++] = (char)c;
        } else {
            int w = snprintf(out + j, out_cap - j, "%%%02X", (unsigned)c);
            if (w < 0 || (size_t)w >= out_cap - j)
                return 0;
            j += (size_t)w;
        }
    }
    if (j >= out_cap)
        return 0;
    out[j] = '\0';
    return j;
}

static hu_error_t teams_load_conversation_history(void *ctx, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t contact_id_len,
                                                  size_t limit, hu_channel_history_entry_t **out,
                                                  size_t *out_count) {
    if (!ctx || !alloc || !contact_id || contact_id_len == 0 || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_teams_ctx_t *c = (hu_teams_ctx_t *)ctx;
    if (!c->graph_access_token || c->graph_access_token_len == 0 || !c->team_id ||
        c->team_id_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;

    if (limit > 50)
        limit = 50;
    if (limit == 0)
        limit = 20;

    char enc_team[512];
    char enc_chan[768];
    if (teams_pct_encode_path_seg(c->team_id, c->team_id_len, enc_team, sizeof(enc_team)) == 0 ||
        teams_pct_encode_path_seg(contact_id, contact_id_len, enc_chan, sizeof(enc_chan)) == 0)
        return HU_ERR_INTERNAL;

    char url_buf[2048];
    int nu = snprintf(url_buf, sizeof(url_buf),
                      TEAMS_GRAPH_API_BASE "/teams/%s/channels/%s/messages?$top=%zu", enc_team,
                      enc_chan, limit);
    if (nu < 0 || (size_t)nu >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->graph_access_token_len,
                      c->graph_access_token);
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

    hu_json_value_t *values = hu_json_object_get(parsed, "value");
    if (!values || values->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    size_t arr_len = values->data.array.len;
    if (arr_len > limit)
        arr_len = limit;
    if (arr_len == 0) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    hu_channel_history_entry_t *entries =
        (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, arr_len * sizeof(*entries));
    if (!entries) {
        hu_json_free(alloc, parsed);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, arr_len * sizeof(*entries));
    size_t count = 0;

    for (size_t i = 0; i < values->data.array.len && count < arr_len; i++) {
        hu_json_value_t *msg = values->data.array.items[i];
        if (!msg || msg->type != HU_JSON_OBJECT)
            continue;
        hu_json_value_t *body_o = hu_json_object_get(msg, "body");
        const char *content = body_o ? hu_json_get_string(body_o, "content") : NULL;
        if (!content || strlen(content) == 0)
            continue;

        entries[count].from_me = false;
        size_t text_len = strlen(content);
        if (text_len > 511)
            text_len = 511;
        memcpy(entries[count].text, content, text_len);
        entries[count].text[text_len] = '\0';

        const char *cdt = hu_json_get_string(msg, "createdDateTime");
        if (cdt && strlen(cdt) >= 16) {
            memcpy(entries[count].timestamp, cdt, 10);
            entries[count].timestamp[10] = ' ';
            memcpy(entries[count].timestamp + 11, cdt + 11, 5);
            entries[count].timestamp[16] = '\0';
        } else {
            entries[count].timestamp[0] = '\0';
        }

        count++;
    }

    hu_json_free(alloc, parsed);

    for (size_t i = 0; i < count / 2; i++) {
        hu_channel_history_entry_t tmp = entries[i];
        entries[i] = entries[count - 1 - i];
        entries[count - 1 - i] = tmp;
    }

    if (count < arr_len) {
        hu_channel_history_entry_t *exact =
            (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, count * sizeof(*entries));
        if (!exact) {
            alloc->free(alloc->ctx, entries, arr_len * sizeof(*entries));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(exact, entries, count * sizeof(*entries));
        alloc->free(alloc->ctx, entries, arr_len * sizeof(*entries));
        entries = exact;
    }

    *out = entries;
    *out_count = count;
    return HU_OK;
}
#else
static hu_error_t teams_load_conversation_history(void *ctx, hu_allocator_t *alloc,
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

static hu_error_t teams_react(void *ctx, const char *target, size_t target_len, int64_t message_id,
                              hu_reaction_type_t reaction) {
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)ctx;
    (void)message_id;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!target || target_len == 0 || reaction == HU_REACTION_NONE)
        return HU_ERR_INVALID_ARGUMENT;
    const char *rtype = teams_graph_reaction_type(reaction);
    if (!rtype)
        return HU_ERR_INVALID_ARGUMENT;
    const char *chat_id = NULL;
    size_t chat_len = 0;
    const char *msg_id = NULL;
    size_t msg_len = 0;
    if (!teams_split_chat_message(target, target_len, &chat_id, &chat_len, &msg_id, &msg_len))
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)rtype;
    (void)chat_id;
    (void)chat_len;
    (void)msg_id;
    (void)msg_len;
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    if (!c->graph_access_token || c->graph_access_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    char url_buf[4096];
    int n = snprintf(url_buf, sizeof(url_buf),
                     TEAMS_GRAPH_API_BASE "/chats/%.*s/messages/%.*s/reactions", (int)chat_len,
                     chat_id, (int)msg_len, msg_id);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;
    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->graph_access_token_len,
                      c->graph_access_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;
    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;
    err = hu_json_buf_append_raw(&jbuf, "{\"reactionType\":", 16);
    if (err)
        goto teams_re_fail;
    err = hu_json_append_string(&jbuf, rtype, strlen(rtype));
    if (err)
        goto teams_re_fail;
    err = hu_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto teams_re_fail;
    hu_http_response_t resp = {0};
    err = hu_http_post_json(c->alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
    hu_json_buf_free(&jbuf);
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    if (err != HU_OK)
        return HU_ERR_CHANNEL_SEND;
    if (resp.status_code < 200 || resp.status_code >= 300)
        return HU_ERR_CHANNEL_SEND;
    return HU_OK;
teams_re_fail:
    hu_json_buf_free(&jbuf);
    return err;
#else
    (void)chat_id;
    (void)chat_len;
    (void)msg_id;
    (void)msg_len;
    (void)rtype;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static bool teams_human_active_recently(void *ctx, const char *contact, size_t contact_len,
                                        int window_sec) {
#if HU_IS_TEST
    (void)ctx;
    (void)contact;
    (void)contact_len;
    (void)window_sec;
    return false;
#else
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)ctx;
    if (!c || !contact || contact_len == 0 || window_sec <= 0)
        return false;
    time_t newest = 0;
    for (size_t i = 0; i < c->queue_count; i++) {
        size_t idx = (c->queue_head + i) % TEAMS_QUEUE_MAX;
        const hu_teams_queued_msg_t *slot = &c->queue[idx];
        size_t skl = strlen(slot->session_key);
        if (skl == contact_len && memcmp(slot->session_key, contact, contact_len) == 0) {
            if (slot->received_at > newest)
                newest = slot->received_at;
        }
    }
    if (newest == 0)
        return false;
    time_t now = time(NULL);
    return (now - newest) <= (time_t)window_sec;
#endif
}

static const hu_channel_vtable_t teams_vtable = {
    .start = teams_start,
    .stop = teams_stop,
    .send = teams_send,
    .name = teams_name,
    .health_check = teams_health_check,
    .send_event = NULL,
    .start_typing = teams_start_typing,
    .stop_typing = teams_stop_typing,
    .load_conversation_history = teams_load_conversation_history,
    .get_response_constraints = teams_get_response_constraints,
    .react = teams_react,
    .reply = NULL,
    .react_emoji = NULL,
    .send_sticker = NULL,
    .human_active_recently = teams_human_active_recently,
};

hu_error_t hu_teams_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                               size_t body_len) {
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)alloc;
    teams_queue_push(c, "test-sender", 11, body, body_len);
    return HU_OK;
#else
    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &parsed);
    if (err != HU_OK || !parsed)
        return HU_OK;
    const char *activity_type = hu_json_get_string(parsed, "type");
    if (activity_type && strcmp(activity_type, "message") == 0) {
        const char *text = hu_json_get_string(parsed, "text");
        hu_json_value_t *from = hu_json_object_get(parsed, "from");
        const char *from_id = from ? hu_json_get_string(from, "id") : NULL;
        if (text && from_id && strlen(text) > 0)
            teams_queue_push(c, from_id, strlen(from_id), text, strlen(text));
    }
    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

hu_error_t hu_teams_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count) {
    (void)alloc;
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)channel_ctx;
    if (!c || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if HU_IS_TEST
    if (c->mock_count > 0) {
        size_t n = c->mock_count < max_msgs ? c->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, c->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, c->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        c->mock_count = 0;
        return HU_OK;
    }
#endif
    size_t cnt = 0;
    while (c->queue_count > 0 && cnt < max_msgs) {
        hu_teams_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % TEAMS_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return HU_OK;
}

bool hu_teams_is_configured(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)ch->ctx;
    return c->webhook_url != NULL && c->webhook_url_len > 0;
}

hu_error_t hu_teams_create(hu_allocator_t *alloc, const char *webhook_url, size_t webhook_url_len,
                           const char *graph_access_token, size_t graph_access_token_len,
                           const char *team_id, size_t team_id_len, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (webhook_url && webhook_url_len > 0) {
        c->webhook_url = hu_strndup(alloc, webhook_url, webhook_url_len);
        if (!c->webhook_url) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->webhook_url_len = webhook_url_len;
    }
    if (graph_access_token && graph_access_token_len > 0) {
        c->graph_access_token = hu_strndup(alloc, graph_access_token, graph_access_token_len);
        if (!c->graph_access_token) {
            if (c->webhook_url)
                hu_str_free(alloc, c->webhook_url);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->graph_access_token_len = graph_access_token_len;
    }
    if (team_id && team_id_len > 0) {
        c->team_id = hu_strndup(alloc, team_id, team_id_len);
        if (!c->team_id) {
            if (c->webhook_url)
                hu_str_free(alloc, c->webhook_url);
            if (c->graph_access_token)
                hu_str_free(alloc, c->graph_access_token);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->team_id_len = team_id_len;
    }
    out->ctx = c;
    out->vtable = &teams_vtable;
    return HU_OK;
}

void hu_teams_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_teams_ctx_t *c = (hu_teams_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (c->webhook_url)
            hu_str_free(a, c->webhook_url);
        if (c->graph_access_token)
            hu_str_free(a, c->graph_access_token);
        if (c->team_id)
            hu_str_free(a, c->team_id);
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_teams_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                     size_t session_key_len, const char *content,
                                     size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)ch->ctx;
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
const char *hu_teams_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_teams_ctx_t *c = (hu_teams_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
