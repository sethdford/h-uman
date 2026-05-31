/* asprintf is a GNU/BSD extension; glibc declares it only under _GNU_SOURCE.
 * macOS declares it unconditionally, so this define is a harmless no-op there. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "human/channels/imessage_private/client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Escape a string for embedding inside a JSON string literal. Returns a
 * heap-allocated NUL-terminated copy (caller frees), or NULL on OOM/NULL input.
 * Handles ", \, and C0 control characters (\n \r \t \b \f as short escapes,
 * the rest as \u00XX). Self-contained — the ML json escaper is HU_ENABLE_ML
 * gated and unavailable to this cross-platform module. */
static char *json_escape_dup(const char *src) {
    if (!src)
        return NULL;
    size_t len = strlen(src);
    /* Worst case is \u00XX (6 bytes) per input byte. */
    char *out = (char *)malloc(len * 6 + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
        case '"':
            out[o++] = '\\';
            out[o++] = '"';
            break;
        case '\\':
            out[o++] = '\\';
            out[o++] = '\\';
            break;
        case '\n':
            out[o++] = '\\';
            out[o++] = 'n';
            break;
        case '\r':
            out[o++] = '\\';
            out[o++] = 'r';
            break;
        case '\t':
            out[o++] = '\\';
            out[o++] = 't';
            break;
        case '\b':
            out[o++] = '\\';
            out[o++] = 'b';
            break;
        case '\f':
            out[o++] = '\\';
            out[o++] = 'f';
            break;
        default:
            if (c < 0x20) {
                o += (size_t)snprintf(out + o, 7, "\\u%04x", c);
            } else {
                out[o++] = (char)c;
            }
            break;
        }
    }
    out[o] = '\0';
    return out;
}

/* Write a formatted command into out; returns HU_OK / BUFFER_TOO_SMALL. */
static hu_error_t emit(char *out, size_t cap, const char *json) {
    size_t n = strlen(json);
    if (n + 1 > cap)
        return HU_ERR_LIMIT_REACHED;
    memcpy(out, json, n + 1);
    return HU_OK;
}

hu_error_t hu_imessage_private_build_send(char *out, size_t cap, const char *txn_id,
                                          const char *chat_guid, const char *text,
                                          const char *parent_guid, int part_index) {
    if (!out || cap == 0 || !txn_id || !chat_guid || !chat_guid[0] || !text)
        return HU_ERR_INVALID_ARGUMENT;

    char *eg = json_escape_dup(chat_guid);
    char *et = json_escape_dup(text);
    char *eid = json_escape_dup(txn_id);
    char *ep = (parent_guid && parent_guid[0]) ? json_escape_dup(parent_guid) : NULL;
    hu_error_t rc = HU_ERR_OUT_OF_MEMORY;
    if (!eg || !et || !eid || ((parent_guid && parent_guid[0]) && !ep))
        goto done;

    char *buf = NULL;
    int need;
    if (ep) {
        need = asprintf(&buf,
                        "{\"action\":\"send-message\",\"data\":{\"chatGuid\":\"%s\","
                        "\"message\":\"%s\",\"selectedMessageGuid\":\"%s\",\"partIndex\":%d},"
                        "\"transactionId\":\"%s\"}",
                        eg, et, ep, part_index, eid);
    } else {
        need = asprintf(&buf,
                        "{\"action\":\"send-message\",\"data\":{\"chatGuid\":\"%s\","
                        "\"message\":\"%s\"},\"transactionId\":\"%s\"}",
                        eg, et, eid);
    }
    if (need < 0 || !buf) {
        rc = HU_ERR_OUT_OF_MEMORY;
        goto done;
    }
    rc = emit(out, cap, buf);
    free(buf);

done:
    free(eg);
    free(et);
    free(eid);
    free(ep);
    return rc;
}

hu_error_t hu_imessage_private_build_reaction(char *out, size_t cap, const char *txn_id,
                                              const char *chat_guid, const char *parent_guid,
                                              const char *reaction_type, int part_index) {
    if (!out || cap == 0 || !txn_id || !chat_guid || !chat_guid[0] || !parent_guid ||
        !parent_guid[0] || !reaction_type || !reaction_type[0])
        return HU_ERR_INVALID_ARGUMENT;

    char *eg = json_escape_dup(chat_guid);
    char *ep = json_escape_dup(parent_guid);
    char *er = json_escape_dup(reaction_type);
    char *eid = json_escape_dup(txn_id);
    hu_error_t rc = HU_ERR_OUT_OF_MEMORY;
    char *buf = NULL;
    if (!eg || !ep || !er || !eid)
        goto done;
    if (asprintf(&buf,
                 "{\"action\":\"send-reaction\",\"data\":{\"chatGuid\":\"%s\","
                 "\"selectedMessageGuid\":\"%s\",\"reactionType\":\"%s\",\"partIndex\":%d},"
                 "\"transactionId\":\"%s\"}",
                 eg, ep, er, part_index, eid) < 0 ||
        !buf)
        goto done;
    rc = emit(out, cap, buf);
    free(buf);
done:
    free(eg);
    free(ep);
    free(er);
    free(eid);
    return rc;
}

hu_error_t hu_imessage_private_build_edit(char *out, size_t cap, const char *txn_id,
                                          const char *chat_guid, const char *message_guid,
                                          const char *edited_text, const char *backcompat_text,
                                          int part_index) {
    if (!out || cap == 0 || !txn_id || !chat_guid || !chat_guid[0] || !message_guid ||
        !message_guid[0] || !edited_text)
        return HU_ERR_INVALID_ARGUMENT;

    char *eg = json_escape_dup(chat_guid);
    char *em = json_escape_dup(message_guid);
    char *ee = json_escape_dup(edited_text);
    char *eb = json_escape_dup(backcompat_text ? backcompat_text : "");
    char *eid = json_escape_dup(txn_id);
    hu_error_t rc = HU_ERR_OUT_OF_MEMORY;
    char *buf = NULL;
    if (!eg || !em || !ee || !eb || !eid)
        goto done;
    if (asprintf(&buf,
                 "{\"action\":\"edit-message\",\"data\":{\"chatGuid\":\"%s\","
                 "\"messageGuid\":\"%s\",\"editedMessage\":\"%s\","
                 "\"backwardsCompatibilityMessage\":\"%s\",\"partIndex\":%d},"
                 "\"transactionId\":\"%s\"}",
                 eg, em, ee, eb, part_index, eid) < 0 ||
        !buf)
        goto done;
    rc = emit(out, cap, buf);
    free(buf);
done:
    free(eg);
    free(em);
    free(ee);
    free(eb);
    free(eid);
    return rc;
}

hu_error_t hu_imessage_private_build_unsend(char *out, size_t cap, const char *txn_id,
                                            const char *chat_guid, const char *message_guid,
                                            int part_index) {
    if (!out || cap == 0 || !txn_id || !chat_guid || !chat_guid[0] || !message_guid ||
        !message_guid[0])
        return HU_ERR_INVALID_ARGUMENT;

    char *eg = json_escape_dup(chat_guid);
    char *em = json_escape_dup(message_guid);
    char *eid = json_escape_dup(txn_id);
    hu_error_t rc = HU_ERR_OUT_OF_MEMORY;
    char *buf = NULL;
    if (!eg || !em || !eid)
        goto done;
    if (asprintf(&buf,
                 "{\"action\":\"unsend-message\",\"data\":{\"chatGuid\":\"%s\","
                 "\"messageGuid\":\"%s\",\"partIndex\":%d},\"transactionId\":\"%s\"}",
                 eg, em, part_index, eid) < 0 ||
        !buf)
        goto done;
    rc = emit(out, cap, buf);
    free(buf);
done:
    free(eg);
    free(em);
    free(eid);
    return rc;
}

bool hu_imessage_private_should_route(bool enabled, hu_imessage_private_mode_t mode,
                                      bool helper_connected) {
    /* Only LIVE + enabled + a connected helper drives IMCore. SHADOW runs/logs
     * but must NOT change the sent output, so it does NOT route here; OFF and
     * not-connected fall through to the Tier-1 reply path. */
    return enabled && mode == HU_IMESSAGE_PRIVATE_MODE_LIVE && helper_connected;
}
