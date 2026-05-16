#include "human/provider/structured_output.h"
#include "human/core/json.h"
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Canonical chat-reply schema
 * ────────────────────────────────────────────────────────────────────────── */

static const char k_chat_reply_schema[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"reply\":{\"type\":\"string\",\"description\":\"The text the persona is sending to the "
    "user\"},"
    "\"reasoning\":{\"type\":\"string\",\"description\":\"Optional internal chain-of-thought; "
    "never sent to the channel\"}"
    "},"
    "\"required\":[\"reply\"]"
    "}";

const char *hu_structured_output_chat_reply_schema(void) {
    return k_chat_reply_schema;
}

size_t hu_structured_output_chat_reply_schema_len(void) {
    return sizeof(k_chat_reply_schema) - 1;
}

/* ──────────────────────────────────────────────────────────────────────────
 * JSON reply extractor
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_structured_output_extract_reply(hu_allocator_t *alloc, const char *body,
                                              size_t body_len, char **out_reply,
                                              size_t *out_reply_len, char **out_reasoning,
                                              size_t *out_reasoning_len) {

    if (!alloc || !body || !out_reply || !out_reply_len) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Zero-length body is definitely not valid JSON */
    if (body_len == 0) {
        return HU_ERR_PARSE;
    }

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &root);
    if (err != HU_OK || root == NULL) {
        return HU_ERR_PARSE;
    }

    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    /* Extract "reply" field — required */
    hu_json_value_t *reply_val = hu_json_object_get(root, "reply");
    if (!reply_val || reply_val->type != HU_JSON_STRING) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    size_t rlen = reply_val->data.string.len;
    char *rcopy = (char *)alloc->alloc(alloc->ctx, rlen + 1);
    if (!rcopy) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(rcopy, reply_val->data.string.ptr, rlen);
    rcopy[rlen] = '\0';
    *out_reply = rcopy;
    *out_reply_len = rlen;

    /* Extract "reasoning" field — optional */
    if (out_reasoning && out_reasoning_len) {
        *out_reasoning = NULL;
        *out_reasoning_len = 0;
        hu_json_value_t *reason_val = hu_json_object_get(root, "reasoning");
        if (reason_val && reason_val->type == HU_JSON_STRING) {
            size_t rslen = reason_val->data.string.len;
            char *rscopy = (char *)alloc->alloc(alloc->ctx, rslen + 1);
            if (rscopy) {
                memcpy(rscopy, reason_val->data.string.ptr, rslen);
                rscopy[rslen] = '\0';
                *out_reasoning = rscopy;
                *out_reasoning_len = rslen;
            }
        }
    }

    hu_json_free(alloc, root);
    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Sentinel fallback extractor
 * ────────────────────────────────────────────────────────────────────────── */

#define REPLY_OPEN      "<REPLY>"
#define REPLY_CLOSE     "</REPLY>"
#define REPLY_OPEN_LEN  (sizeof(REPLY_OPEN) - 1)
#define REPLY_CLOSE_LEN (sizeof(REPLY_CLOSE) - 1)

hu_error_t hu_structured_output_extract_sentinel(hu_allocator_t *alloc, const char *body,
                                                 size_t body_len, char **out_reply,
                                                 size_t *out_reply_len) {

    if (!alloc || !body || !out_reply || !out_reply_len) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (body_len < REPLY_OPEN_LEN + REPLY_CLOSE_LEN) {
        return HU_ERR_PARSE;
    }

    /* Find opening marker */
    const char *open_pos = NULL;
    for (size_t i = 0; i + REPLY_OPEN_LEN <= body_len; i++) {
        if (memcmp(body + i, REPLY_OPEN, REPLY_OPEN_LEN) == 0) {
            open_pos = body + i;
            break;
        }
    }
    if (!open_pos) {
        return HU_ERR_PARSE;
    }

    const char *content_start = open_pos + REPLY_OPEN_LEN;
    size_t remaining = body_len - (size_t)(content_start - body);

    /* Find closing marker */
    const char *close_pos = NULL;
    for (size_t i = 0; i + REPLY_CLOSE_LEN <= remaining; i++) {
        if (memcmp(content_start + i, REPLY_CLOSE, REPLY_CLOSE_LEN) == 0) {
            close_pos = content_start + i;
            break;
        }
    }
    if (!close_pos) {
        return HU_ERR_PARSE;
    }

    size_t inner_len = (size_t)(close_pos - content_start);
    char *copy = (char *)alloc->alloc(alloc->ctx, inner_len + 1);
    if (!copy) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, content_start, inner_len);
    copy[inner_len] = '\0';
    *out_reply = copy;
    *out_reply_len = inner_len;
    return HU_OK;
}
