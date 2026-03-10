#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/persona.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

hu_error_t hu_persona_sampler_imessage_query(char *buf, size_t cap, size_t *out_len, size_t limit) {
    if (!buf || !out_len || cap < 64)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                     "SELECT text, handle_id, date, is_from_me FROM message "
                     "WHERE is_from_me = 1 AND text IS NOT NULL AND text != '' "
                     "ORDER BY date DESC LIMIT %zu",
                     limit);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

/* Facebook data export format: {"messages": [{"sender_name": "...", "content": "...", ...}, ...]}
 * We extract messages where sender_name matches the first sender we see (owner heuristic). */
hu_error_t hu_persona_sampler_facebook_parse(const char *json, size_t json_len, char ***out,
                                             size_t *out_count) {
    if (!json || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, json_len, &root);
    if (err != HU_OK || !root)
        return err != HU_OK ? err : HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *messages = hu_json_object_get(root, "messages");
    if (!messages || messages->type != HU_JSON_ARRAY) {
        hu_json_free(&alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t arr_len = messages->data.array.len;
    if (arr_len == 0) {
        hu_json_free(&alloc, root);
        return HU_OK;
    }

    /* First pass: find the most frequent sender (owner heuristic) */
    const char *owner = NULL;
    size_t owner_len = 0;
    size_t owner_count = 0;

    for (size_t i = 0; i < arr_len; i++) {
        hu_json_value_t *msg = messages->data.array.items[i];
        if (!msg || msg->type != HU_JSON_OBJECT)
            continue;
        const char *sender = hu_json_get_string(msg, "sender_name");
        if (!sender)
            continue;
        size_t slen = strlen(sender);

        size_t count = 0;
        for (size_t j = 0; j < arr_len; j++) {
            hu_json_value_t *m2 = messages->data.array.items[j];
            if (!m2 || m2->type != HU_JSON_OBJECT)
                continue;
            const char *s2 = hu_json_get_string(m2, "sender_name");
            if (s2 && strlen(s2) == slen && memcmp(s2, sender, slen) == 0)
                count++;
        }
        if (count > owner_count) {
            owner = sender;
            owner_len = slen;
            owner_count = count;
        }
    }

    if (!owner || owner_count == 0) {
        hu_json_free(&alloc, root);
        return HU_OK;
    }

    /* Second pass: collect content strings from the owner */
    size_t cap = owner_count < 64 ? owner_count : 64;
    char **results = (char **)alloc.alloc(alloc.ctx, cap * sizeof(char *));
    if (!results) {
        hu_json_free(&alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    for (size_t i = 0; i < arr_len && count < cap; i++) {
        hu_json_value_t *msg = messages->data.array.items[i];
        if (!msg || msg->type != HU_JSON_OBJECT)
            continue;
        const char *sender = hu_json_get_string(msg, "sender_name");
        if (!sender || strlen(sender) != owner_len || memcmp(sender, owner, owner_len) != 0)
            continue;
        const char *content = hu_json_get_string(msg, "content");
        if (!content || content[0] == '\0')
            continue;
        results[count] = hu_strdup(&alloc, content);
        if (!results[count]) {
            for (size_t j = 0; j < count; j++)
                alloc.free(alloc.ctx, results[j], strlen(results[j]) + 1);
            alloc.free(alloc.ctx, results, cap * sizeof(char *));
            hu_json_free(&alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        count++;
    }

    hu_json_free(&alloc, root);
    *out = results;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_persona_sampler_imessage_conversation_query(const char *handle_id,
                                                          size_t handle_id_len, char *buf,
                                                          size_t cap, size_t *out_len,
                                                          size_t limit) {
    if (!handle_id || handle_id_len == 0 || !buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    /* Sanitize handle_id: escape single quotes by doubling them (SQL escaping). */
    char sanitized[257];
    size_t out_pos = 0;
    for (size_t i = 0; i < handle_id_len && out_pos < 256; i++) {
        if (handle_id[i] == '\'') {
            if (out_pos + 2 > 256)
                break;
            sanitized[out_pos++] = '\'';
            sanitized[out_pos++] = '\'';
        } else {
            if (out_pos + 1 > 256)
                break;
            sanitized[out_pos++] = handle_id[i];
        }
    }
    sanitized[out_pos] = '\0';

    int n = snprintf(buf, cap,
                     "SELECT m.text, m.is_from_me, m.date "
                     "FROM message m "
                     "JOIN chat_message_join cmj ON cmj.message_id = m.ROWID "
                     "JOIN chat c ON c.ROWID = cmj.chat_id "
                     "JOIN chat_handle_join chj ON chj.chat_id = c.ROWID "
                     "JOIN handle h ON h.ROWID = chj.handle_id "
                     "WHERE h.id = '%s' AND m.text IS NOT NULL AND m.text != '' "
                     "ORDER BY m.date ASC LIMIT %zu",
                     sanitized, limit);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_persona_sampler_build_examples(hu_allocator_t *alloc,
                                             const hu_sampler_raw_msg_t *msgs, size_t msg_count,
                                             hu_persona_example_t **out, size_t *out_count) {
    if (!alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (!msgs || msg_count == 0)
        return HU_OK;

    size_t cap = msg_count < 64 ? msg_count : 64;
    hu_persona_example_t *examples =
        (hu_persona_example_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_persona_example_t));
    if (!examples)
        return HU_ERR_OUT_OF_MEMORY;
    memset(examples, 0, cap * sizeof(hu_persona_example_t));
    size_t count = 0;

    for (size_t i = 0; i < msg_count && count < cap; i++) {
        if (msgs[i].is_from_me || !msgs[i].text || msgs[i].text_len == 0)
            continue;

        const char *incoming = msgs[i].text;
        size_t incoming_len = msgs[i].text_len;
        int64_t incoming_ts = msgs[i].timestamp;

        char response_buf[1024];
        size_t rpos = 0;
        size_t j = i + 1;
        while (j < msg_count && msgs[j].is_from_me) {
            if (msgs[j].text && msgs[j].text_len > 0) {
                int64_t gap = msgs[j].timestamp - incoming_ts;
                if (gap > 1800)
                    break;
                if (rpos > 0 && rpos < sizeof(response_buf) - 1)
                    response_buf[rpos++] = '\n';
                size_t copy = msgs[j].text_len;
                if (rpos + copy >= sizeof(response_buf))
                    copy = sizeof(response_buf) - rpos - 1;
                memcpy(response_buf + rpos, msgs[j].text, copy);
                rpos += copy;
            }
            j++;
        }
        if (rpos == 0)
            continue;
        response_buf[rpos] = '\0';

        examples[count].context = hu_strndup(alloc, "texting conversation", 20);
        examples[count].incoming = hu_strndup(alloc, incoming, incoming_len);
        examples[count].response = hu_strndup(alloc, response_buf, rpos);
        if (!examples[count].context || !examples[count].incoming || !examples[count].response) {
            for (size_t k = 0; k <= count; k++) {
                if (examples[k].context)
                    alloc->free(alloc->ctx, examples[k].context, strlen(examples[k].context) + 1);
                if (examples[k].incoming)
                    alloc->free(alloc->ctx, examples[k].incoming, strlen(examples[k].incoming) + 1);
                if (examples[k].response)
                    alloc->free(alloc->ctx, examples[k].response, strlen(examples[k].response) + 1);
            }
            alloc->free(alloc->ctx, examples, cap * sizeof(hu_persona_example_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        count++;
        i = j > 0 ? j - 1 : i;
    }

    *out = examples;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_persona_sampler_detect_contact(hu_allocator_t *alloc,
                                             const hu_sampler_raw_msg_t *msgs, size_t msg_count,
                                             hu_sampler_contact_stats_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    (void)alloc;
    if (!msgs || msg_count == 0)
        return HU_OK;

    size_t their_count = 0, my_count = 0;
    size_t total_their_len = 0, total_my_len = 0;
    size_t emoji_msgs = 0, link_msgs = 0;
    size_t burst_count = 0;

    for (size_t i = 0; i < msg_count; i++) {
        if (!msgs[i].text || msgs[i].text_len == 0)
            continue;
        if (msgs[i].is_from_me) {
            my_count++;
            total_my_len += msgs[i].text_len;
        } else {
            their_count++;
            total_their_len += msgs[i].text_len;
        }
        for (size_t c = 0; c < msgs[i].text_len; c++) {
            unsigned char ch = (unsigned char)msgs[i].text[c];
            if (ch >= 0xF0) {
                emoji_msgs++;
                break;
            }
        }
        if (msgs[i].text_len > 7) {
            const char *t = msgs[i].text;
            if (strstr(t, "http://") || strstr(t, "https://"))
                link_msgs++;
        }
        if (i > 0 && !msgs[i].is_from_me && !msgs[i - 1].is_from_me) {
            int64_t gap = msgs[i].timestamp - msgs[i - 1].timestamp;
            if (gap < 30)
                burst_count++;
        }
    }

    out->their_msg_count = their_count;
    out->my_msg_count = my_count;
    if (their_count > 0)
        out->avg_their_len = total_their_len / their_count;
    if (my_count > 0)
        out->avg_my_len = total_my_len / my_count;
    out->uses_emoji = (emoji_msgs > msg_count / 5);
    out->sends_links = (link_msgs > msg_count / 10);
    out->texts_in_bursts = (burst_count > their_count / 4);
    out->prefers_short = (out->avg_their_len < 40);
    return HU_OK;
}

/* Gmail Takeout JSON format: {"messages": [{"from": "me", "body": "..."}, ...]}
 * We extract messages where "from" is "me" (user's sent messages). */
hu_error_t hu_persona_sampler_gmail_parse(const char *json, size_t json_len, char ***out,
                                          size_t *out_count) {
    if (!json || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, json_len, &root);
    if (err != HU_OK || !root)
        return err != HU_OK ? err : HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *messages = hu_json_object_get(root, "messages");
    if (!messages || messages->type != HU_JSON_ARRAY) {
        hu_json_free(&alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t arr_len = messages->data.array.len;
    if (arr_len == 0) {
        hu_json_free(&alloc, root);
        return HU_OK;
    }

    /* Count "me" messages first */
    size_t me_count = 0;
    for (size_t i = 0; i < arr_len; i++) {
        hu_json_value_t *msg = messages->data.array.items[i];
        if (!msg || msg->type != HU_JSON_OBJECT)
            continue;
        const char *from = hu_json_get_string(msg, "from");
        if (from && strcmp(from, "me") == 0) {
            const char *body = hu_json_get_string(msg, "body");
            if (body && body[0] != '\0')
                me_count++;
        }
    }

    if (me_count == 0) {
        hu_json_free(&alloc, root);
        return HU_OK;
    }

    size_t cap = me_count < 64 ? me_count : 64;
    char **results = (char **)alloc.alloc(alloc.ctx, cap * sizeof(char *));
    if (!results) {
        hu_json_free(&alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    for (size_t i = 0; i < arr_len && count < cap; i++) {
        hu_json_value_t *msg = messages->data.array.items[i];
        if (!msg || msg->type != HU_JSON_OBJECT)
            continue;
        const char *from = hu_json_get_string(msg, "from");
        if (!from || strcmp(from, "me") != 0)
            continue;
        const char *body = hu_json_get_string(msg, "body");
        if (!body || body[0] == '\0')
            continue;
        results[count] = hu_strdup(&alloc, body);
        if (!results[count]) {
            for (size_t j = 0; j < count; j++)
                alloc.free(alloc.ctx, results[j], strlen(results[j]) + 1);
            alloc.free(alloc.ctx, results, cap * sizeof(char *));
            hu_json_free(&alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        count++;
    }

    hu_json_free(&alloc, root);
    *out = results;
    *out_count = count;
    return HU_OK;
}
