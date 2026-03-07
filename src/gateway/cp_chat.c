/* Chat-related control protocol handlers: chat.send, chat.history, chat.abort */
#include "cp_internal.h"
#include "seaclaw/bus.h"
#include "seaclaw/session.h"
#include <stdio.h>
#include <string.h>

sc_error_t cp_chat_send(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                        const sc_control_protocol_t *proto, const sc_json_value_t *root, char **out,
                        size_t *out_len) {
    (void)conn;
    (void)proto;
    const char *message = NULL;
    const char *session_key = "default";

    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *m = sc_json_get_string(params, "message");
            if (m)
                message = m;
            const char *sk = sc_json_get_string(params, "sessionKey");
            if (sk)
                session_key = sk;
        }
    }

    if (!message || !message[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "status", "rejected");
        cp_json_set_str(alloc, obj, "error", "message is required");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }

    if (app && app->bus) {
        sc_bus_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SC_BUS_MESSAGE_RECEIVED;
        snprintf(ev.channel, SC_BUS_CHANNEL_LEN, "control-ui");
        snprintf(ev.id, SC_BUS_ID_LEN, "%s", session_key);
        ev.payload = (void *)message;
        size_t ml = strlen(message);
        if (ml >= SC_BUS_MSG_LEN)
            ml = SC_BUS_MSG_LEN - 1;
        memcpy(ev.message, message, ml);
        ev.message[ml] = '\0';
        sc_bus_publish(app->bus, &ev);
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "status", "queued");
    cp_json_set_str(alloc, obj, "sessionKey", session_key);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

sc_error_t cp_chat_history(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                           const sc_control_protocol_t *proto, const sc_json_value_t *root,
                           char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *msgs = sc_json_array_new(alloc);

    const char *session_key = "default";
    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *sk = sc_json_get_string(params, "sessionKey");
            if (sk)
                session_key = sk;
        }
    }

    if (app && app->sessions) {
        sc_session_t *sess = sc_session_get_or_create(app->sessions, session_key);
        if (sess) {
            for (size_t i = 0; i < sess->message_count; i++) {
                sc_json_value_t *m_obj = sc_json_object_new(alloc);
                cp_json_set_str(alloc, m_obj, "role", sess->messages[i].role);
                if (sess->messages[i].content)
                    cp_json_set_str(alloc, m_obj, "content", sess->messages[i].content);
                sc_json_array_push(alloc, msgs, m_obj);
            }
        }
    }

    sc_json_object_set(alloc, obj, "messages", msgs);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

sc_error_t cp_chat_abort(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                         const sc_control_protocol_t *proto, const sc_json_value_t *root,
                         char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    bool aborted = false;
    if (app && app->bus) {
        sc_bus_publish_simple(app->bus, SC_BUS_ERROR, "control-ui", "", "chat.abort");
        aborted = true;
    }
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "aborted", sc_json_bool_new(alloc, aborted));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}
