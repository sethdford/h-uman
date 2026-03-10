/* Chat-related control protocol handlers: chat.send, chat.history, chat.abort */
#include "cp_internal.h"
#include "human/bus.h"
#include "human/session.h"
#include <stdio.h>
#include <string.h>

hu_error_t cp_chat_send(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                        const hu_control_protocol_t *proto, const hu_json_value_t *root, char **out,
                        size_t *out_len) {
    (void)conn;
    (void)proto;
    const char *message = NULL;
    const char *session_key = "default";

    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *m = hu_json_get_string(params, "message");
            if (m)
                message = m;
            const char *sk = hu_json_get_string(params, "sessionKey");
            if (sk)
                session_key = sk;
        }
    }

    if (!message || !message[0]) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "status", "rejected");
        cp_json_set_str(alloc, obj, "error", "message is required");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }

    if (app && app->bus) {
        hu_bus_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = HU_BUS_MESSAGE_RECEIVED;
        snprintf(ev.channel, HU_BUS_CHANNEL_LEN, "control-ui");
        snprintf(ev.id, HU_BUS_ID_LEN, "%s", session_key);
        ev.payload = (void *)message;
        size_t ml = strlen(message);
        if (ml >= HU_BUS_MSG_LEN)
            ml = HU_BUS_MSG_LEN - 1;
        memcpy(ev.message, message, ml);
        ev.message[ml] = '\0';
        hu_bus_publish(app->bus, &ev);
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "status", "queued");
    cp_json_set_str(alloc, obj, "sessionKey", session_key);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_chat_history(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *msgs = hu_json_array_new(alloc);

    const char *session_key = "default";
    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *sk = hu_json_get_string(params, "sessionKey");
            if (sk)
                session_key = sk;
        }
    }

    if (app && app->sessions) {
        hu_session_t *sess = hu_session_get_or_create(app->sessions, session_key);
        if (sess) {
            for (size_t i = 0; i < sess->message_count; i++) {
                hu_json_value_t *m_obj = hu_json_object_new(alloc);
                cp_json_set_str(alloc, m_obj, "role", sess->messages[i].role);
                if (sess->messages[i].content)
                    cp_json_set_str(alloc, m_obj, "content", sess->messages[i].content);
                hu_json_array_push(alloc, msgs, m_obj);
            }
        }
    }

    hu_json_object_set(alloc, obj, "messages", msgs);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_chat_abort(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                         const hu_control_protocol_t *proto, const hu_json_value_t *root,
                         char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    bool aborted = false;
    if (app && app->bus) {
        hu_bus_publish_simple(app->bus, HU_BUS_ERROR, "control-ui", "", "chat.abort");
        aborted = true;
    }
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "aborted", hu_json_bool_new(alloc, aborted));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}
