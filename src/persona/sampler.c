#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include <stdio.h>
#include <string.h>

sc_error_t sc_persona_sampler_imessage_query(char *buf, size_t cap, size_t *out_len, size_t limit) {
    if (!buf || !out_len || cap < 64)
        return SC_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                     "SELECT text, handle_id, date, is_from_me FROM message "
                     "WHERE is_from_me = 1 AND text IS NOT NULL AND text != '' "
                     "ORDER BY date DESC LIMIT %zu",
                     limit);
    if (n < 0 || (size_t)n >= cap)
        return SC_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return SC_OK;
}

/* Facebook data export format: {"messages": [{"sender_name": "...", "content": "...", ...}, ...]}
 * We extract messages where sender_name matches the first sender we see (owner heuristic). */
sc_error_t sc_persona_sampler_facebook_parse(const char *json, size_t json_len, char ***out,
                                             size_t *out_count) {
    if (!json || !out || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(&alloc, json, json_len, &root);
    if (err != SC_OK || !root)
        return err != SC_OK ? err : SC_ERR_INVALID_ARGUMENT;

    sc_json_value_t *messages = sc_json_object_get(root, "messages");
    if (!messages || messages->type != SC_JSON_ARRAY) {
        sc_json_free(&alloc, root);
        return SC_ERR_INVALID_ARGUMENT;
    }

    size_t arr_len = messages->data.array.len;
    if (arr_len == 0) {
        sc_json_free(&alloc, root);
        return SC_OK;
    }

    /* First pass: find the most frequent sender (owner heuristic) */
    const char *owner = NULL;
    size_t owner_len = 0;
    size_t owner_count = 0;

    for (size_t i = 0; i < arr_len; i++) {
        sc_json_value_t *msg = messages->data.array.items[i];
        if (!msg || msg->type != SC_JSON_OBJECT)
            continue;
        const char *sender = sc_json_get_string(msg, "sender_name");
        if (!sender)
            continue;
        size_t slen = strlen(sender);

        size_t count = 0;
        for (size_t j = 0; j < arr_len; j++) {
            sc_json_value_t *m2 = messages->data.array.items[j];
            if (!m2 || m2->type != SC_JSON_OBJECT)
                continue;
            const char *s2 = sc_json_get_string(m2, "sender_name");
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
        sc_json_free(&alloc, root);
        return SC_OK;
    }

    /* Second pass: collect content strings from the owner */
    size_t cap = owner_count < 64 ? owner_count : 64;
    char **results = (char **)alloc.alloc(alloc.ctx, cap * sizeof(char *));
    if (!results) {
        sc_json_free(&alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    for (size_t i = 0; i < arr_len && count < cap; i++) {
        sc_json_value_t *msg = messages->data.array.items[i];
        if (!msg || msg->type != SC_JSON_OBJECT)
            continue;
        const char *sender = sc_json_get_string(msg, "sender_name");
        if (!sender || strlen(sender) != owner_len || memcmp(sender, owner, owner_len) != 0)
            continue;
        const char *content = sc_json_get_string(msg, "content");
        if (!content || content[0] == '\0')
            continue;
        results[count] = sc_strdup(&alloc, content);
        if (!results[count]) {
            for (size_t j = 0; j < count; j++)
                alloc.free(alloc.ctx, results[j], strlen(results[j]) + 1);
            alloc.free(alloc.ctx, results, cap * sizeof(char *));
            sc_json_free(&alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        count++;
    }

    sc_json_free(&alloc, root);
    *out = results;
    *out_count = count;
    return SC_OK;
}

/* Gmail Takeout JSON format: {"messages": [{"from": "me", "body": "..."}, ...]}
 * We extract messages where "from" is "me" (user's sent messages). */
sc_error_t sc_persona_sampler_gmail_parse(const char *json, size_t json_len, char ***out,
                                          size_t *out_count) {
    if (!json || !out || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(&alloc, json, json_len, &root);
    if (err != SC_OK || !root)
        return err != SC_OK ? err : SC_ERR_INVALID_ARGUMENT;

    sc_json_value_t *messages = sc_json_object_get(root, "messages");
    if (!messages || messages->type != SC_JSON_ARRAY) {
        sc_json_free(&alloc, root);
        return SC_ERR_INVALID_ARGUMENT;
    }

    size_t arr_len = messages->data.array.len;
    if (arr_len == 0) {
        sc_json_free(&alloc, root);
        return SC_OK;
    }

    /* Count "me" messages first */
    size_t me_count = 0;
    for (size_t i = 0; i < arr_len; i++) {
        sc_json_value_t *msg = messages->data.array.items[i];
        if (!msg || msg->type != SC_JSON_OBJECT)
            continue;
        const char *from = sc_json_get_string(msg, "from");
        if (from && strcmp(from, "me") == 0) {
            const char *body = sc_json_get_string(msg, "body");
            if (body && body[0] != '\0')
                me_count++;
        }
    }

    if (me_count == 0) {
        sc_json_free(&alloc, root);
        return SC_OK;
    }

    size_t cap = me_count < 64 ? me_count : 64;
    char **results = (char **)alloc.alloc(alloc.ctx, cap * sizeof(char *));
    if (!results) {
        sc_json_free(&alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    for (size_t i = 0; i < arr_len && count < cap; i++) {
        sc_json_value_t *msg = messages->data.array.items[i];
        if (!msg || msg->type != SC_JSON_OBJECT)
            continue;
        const char *from = sc_json_get_string(msg, "from");
        if (!from || strcmp(from, "me") != 0)
            continue;
        const char *body = sc_json_get_string(msg, "body");
        if (!body || body[0] == '\0')
            continue;
        results[count] = sc_strdup(&alloc, body);
        if (!results[count]) {
            for (size_t j = 0; j < count; j++)
                alloc.free(alloc.ctx, results[j], strlen(results[j]) + 1);
            alloc.free(alloc.ctx, results, cap * sizeof(char *));
            sc_json_free(&alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        count++;
    }

    sc_json_free(&alloc, root);
    *out = results;
    *out_count = count;
    return SC_OK;
}
