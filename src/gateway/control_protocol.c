#include "seaclaw/gateway/control_protocol.h"
#include <stdint.h>
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include <stdio.h>
#include <string.h>

#ifdef SC_GATEWAY_POSIX

static sc_error_t build_connect_response(sc_allocator_t *alloc, char **out,
    size_t *out_len) {
    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root) return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *type_v = sc_json_string_new(alloc, "hello-ok", 8);
    sc_json_value_t *server = sc_json_object_new(alloc);
    sc_json_value_t *version_v = sc_json_string_new(alloc, "0.1.0", 5);
    sc_json_value_t *features = sc_json_object_new(alloc);
    sc_json_value_t *methods_arr = sc_json_array_new(alloc);

    if (!type_v || !server || !version_v || !features || !methods_arr) {
        if (type_v) sc_json_free(alloc, type_v);
        if (server) sc_json_free(alloc, server);
        if (version_v) sc_json_free(alloc, version_v);
        if (features) sc_json_free(alloc, features);
        if (methods_arr) sc_json_free(alloc, methods_arr);
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }

    sc_json_object_set(alloc, server, "version", version_v);
    sc_json_object_set(alloc, root, "type", type_v);
    sc_json_object_set(alloc, root, "server", server);
    sc_json_object_set(alloc, root, "protocol", sc_json_number_new(alloc, 1));

    static const char *const methods[] = {
        "connect", "health", "config.get", "config.schema", "capabilities",
        "chat.send", "chat.history", "chat.abort",
        "config.set", "config.apply", "sessions.list", "sessions.patch",
        "sessions.delete", "tools.catalog", "channels.status", "cron.list",
        "cron.add", "cron.remove", "cron.run", "skills.list", "skills.enable",
        "skills.disable", "update.check", "update.run", "exec.approval.resolve",
        "usage.summary"
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        sc_json_value_t *m = sc_json_string_new(alloc, methods[i],
            (size_t)strlen(methods[i]));
        if (m) sc_json_array_push(alloc, methods_arr, m);
    }
    sc_json_object_set(alloc, features, "methods", methods_arr);
    sc_json_object_set(alloc, root, "features", features);

    sc_error_t err = sc_json_stringify(alloc, root, out, out_len);
    sc_json_free(alloc, root);
    return err;
}

static sc_error_t build_method_response(sc_allocator_t *alloc,
    const char *method, const sc_json_value_t *root,
    char **payload_out, size_t *payload_len_out) {
    *payload_out = NULL;
    *payload_len_out = 0;

    if (strcmp(method, "connect") == 0) {
        return build_connect_response(alloc, payload_out, payload_len_out);
    }

    if (strcmp(method, "health") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "status",
            sc_json_string_new(alloc, "ok", 2));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "config.get") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "exists", sc_json_bool_new(alloc, false));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "config.schema") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "schema", sc_json_object_new(alloc));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "capabilities") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "version",
            sc_json_string_new(alloc, "0.1.0", 5));
        sc_json_object_set(alloc, obj, "tools", sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "channels", sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "providers", sc_json_number_new(alloc, 0));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "chat.send") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "status", sc_json_string_new(alloc, "queued", 6));
        sc_json_object_set(alloc, obj, "sessionKey", sc_json_string_new(alloc, "default", 7));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "chat.history") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "messages", sc_json_array_new(alloc));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "chat.abort") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "aborted", sc_json_bool_new(alloc, true));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "config.set") == 0) {
        if (root) {
            sc_json_value_t *params = sc_json_object_get(root, "params");
            if (params) {
                (void)sc_json_get_string(params, "raw");
                (void)sc_json_get_string(params, "baseHash");
            }
        }
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "saved", sc_json_bool_new(alloc, true));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "config.apply") == 0) {
        if (root) {
            sc_json_value_t *params = sc_json_object_get(root, "params");
            if (params) {
                (void)sc_json_get_string(params, "raw");
                (void)sc_json_get_string(params, "baseHash");
            }
        }
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "applied", sc_json_bool_new(alloc, true));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "sessions.list") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "sessions", sc_json_array_new(alloc));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "sessions.patch") == 0) {
        if (root) {
            sc_json_value_t *params = sc_json_object_get(root, "params");
            if (params) {
                (void)sc_json_get_string(params, "key");
                (void)sc_json_get_string(params, "label");
            }
        }
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "patched", sc_json_bool_new(alloc, true));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "sessions.delete") == 0) {
        if (root) {
            sc_json_value_t *params = sc_json_object_get(root, "params");
            if (params) {
                (void)sc_json_get_string(params, "key");
            }
        }
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "deleted", sc_json_bool_new(alloc, true));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "tools.catalog") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "tools", sc_json_array_new(alloc));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "channels.status") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "channels", sc_json_array_new(alloc));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "cron.list") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "jobs", sc_json_array_new(alloc));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "cron.add") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "added", sc_json_bool_new(alloc, true));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "cron.remove") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "removed", sc_json_bool_new(alloc, true));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "cron.run") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "started", sc_json_bool_new(alloc, true));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "skills.list") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "skills", sc_json_array_new(alloc));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "skills.enable") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "enabled", sc_json_bool_new(alloc, true));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "skills.disable") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "disabled", sc_json_bool_new(alloc, true));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "update.check") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "available", sc_json_bool_new(alloc, false));
        sc_json_object_set(alloc, obj, "current", sc_json_string_new(alloc, "0.1.0", 5));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "update.run") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "status", sc_json_string_new(alloc, "up_to_date", 10));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "exec.approval.resolve") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "resolved", sc_json_bool_new(alloc, true));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    if (strcmp(method, "usage.summary") == 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, obj, "session_cost_usd", sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "daily_cost_usd", sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "monthly_cost_usd", sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "total_tokens", sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "request_count", sc_json_number_new(alloc, 0));
        sc_error_t err = sc_json_stringify(alloc, obj, payload_out, payload_len_out);
        sc_json_free(alloc, obj);
        return err;
    }

    return SC_ERR_NOT_FOUND;
}

#endif /* SC_GATEWAY_POSIX */

void sc_control_protocol_init(sc_control_protocol_t *proto,
    sc_allocator_t *alloc, sc_ws_server_t *ws) {
    if (!proto) return;
    proto->alloc = alloc;
    proto->ws = ws;
    proto->event_seq = 0;
    proto->app_ctx = NULL;
}

void sc_control_protocol_deinit(sc_control_protocol_t *proto) {
    (void)proto;
}

void sc_control_on_message(sc_ws_conn_t *conn, const char *data, size_t data_len,
    void *ctx) {
    sc_control_protocol_t *proto = (sc_control_protocol_t *)ctx;
    if (!proto || !proto->alloc || !proto->ws) return;

#ifdef SC_GATEWAY_POSIX
    sc_json_value_t *root = NULL;
    if (sc_json_parse(proto->alloc, data, data_len, &root) != SC_OK)
        return;
    if (!root || root->type != SC_JSON_OBJECT) {
        if (root) sc_json_free(proto->alloc, root);
        return;
    }

    const char *type_str = sc_json_get_string(root, "type");
    if (!type_str || strcmp(type_str, "req") != 0) {
        sc_json_free(proto->alloc, root);
        return;
    }

    const char *id = sc_json_get_string(root, "id");
    const char *method = sc_json_get_string(root, "method");

    if (!id || !method) {
        sc_json_free(proto->alloc, root);
        return;
    }

    char *payload = NULL;
    size_t payload_len = 0;
    sc_error_t err = build_method_response(proto->alloc, method, root,
        &payload, &payload_len);
    sc_json_free(proto->alloc, root);

    bool ok = (err == SC_OK);
    if (!payload && ok) {
        sc_json_value_t *empty = sc_json_object_new(proto->alloc);
        if (empty) {
            sc_json_stringify(proto->alloc, empty, &payload, &payload_len);
            sc_json_free(proto->alloc, empty);
        }
        if (!payload) payload = (char *)proto->alloc->alloc(proto->alloc->ctx, 3);
        if (payload && !payload_len) {
            memcpy(payload, "{}", 3);
            payload_len = 2;
        }
    } else if (err != SC_OK) {
        payload = (char *)proto->alloc->alloc(proto->alloc->ctx, 64);
        if (payload) {
            size_t n = 0;
            if (err == SC_ERR_NOT_FOUND) {
                n = (size_t)snprintf(payload, 64, "{\"error\":\"unknown method\"}");
            } else {
                n = (size_t)snprintf(payload, 64, "{\"error\":\"internal\"}");
            }
            payload_len = n;
        }
    }

    if (payload) {
        size_t res_cap = 256 + payload_len;
        char *res_buf = (char *)proto->alloc->alloc(proto->alloc->ctx, res_cap);
        if (res_buf) {
            size_t pos = 0;
            pos += (size_t)snprintf(res_buf, res_cap,
                "{\"type\":\"res\",\"id\":\"");
            size_t id_len = strlen(id);
            size_t esc_len = id_len * 2 + 16;
            char *id_esc = (char *)proto->alloc->alloc(proto->alloc->ctx, esc_len);
            if (id_esc) {
                size_t j = 0;
                for (size_t i = 0; i < id_len && j + 4 < esc_len; i++) {
                    char c = id[i];
                    if (c == '"' || c == '\\') {
                        id_esc[j++] = '\\';
                        id_esc[j++] = c;
                    } else {
                        id_esc[j++] = c;
                    }
                }
                id_esc[j] = '\0';
                pos += (size_t)snprintf(res_buf + pos, res_cap - pos, "%s\"", id_esc);
                proto->alloc->free(proto->alloc->ctx, id_esc, esc_len);
            }
            pos += (size_t)snprintf(res_buf + pos, res_cap - pos,
                ",\"ok\":%s,\"payload\":", ok ? "true" : "false");
            memcpy(res_buf + pos, payload, payload_len);
            pos += payload_len;
            res_buf[pos++] = '}';
            res_buf[pos] = '\0';
            sc_ws_server_send(conn, res_buf, pos);
            proto->alloc->free(proto->alloc->ctx, res_buf, res_cap);
        }
        if (err == SC_OK && payload)
            proto->alloc->free(proto->alloc->ctx, payload, payload_len + 1);
        else if (payload && err != SC_OK)
            proto->alloc->free(proto->alloc->ctx, payload, 64);
    }
#endif
}

void sc_control_on_close(sc_ws_conn_t *conn, void *ctx) {
    (void)conn;
    (void)ctx;
}

void sc_control_send_event(sc_control_protocol_t *proto, const char *event_name,
    const char *payload_json) {
    if (!proto || !proto->ws || !event_name) return;

#ifdef SC_GATEWAY_POSIX
    uint64_t seq = proto->event_seq++;
    const char *payload = payload_json ? payload_json : "{}";
    size_t payload_len = strlen(payload);
    size_t event_len = strlen(event_name);

    size_t cap = 128 + event_len + payload_len;
    char *buf = (char *)proto->alloc->alloc(proto->alloc->ctx, cap);
    if (!buf) return;

    int n = snprintf(buf, cap,
        "{\"type\":\"event\",\"event\":\"%s\",\"payload\":%s,\"seq\":%llu}",
        event_name, payload, (unsigned long long)seq);

    if (n > 0 && (size_t)n < cap)
        sc_ws_server_broadcast(proto->ws, buf, (size_t)n);

    proto->alloc->free(proto->alloc->ctx, buf, cap);
#endif
}

sc_error_t sc_control_send_response(sc_ws_conn_t *conn, const char *id,
    bool ok, const char *payload_json) {
    if (!conn || !id) return SC_ERR_INVALID_ARGUMENT;

#ifdef SC_GATEWAY_POSIX
    sc_allocator_t alloc = sc_system_allocator();
    const char *payload = payload_json ? payload_json : "{}";
    size_t payload_len = strlen(payload);
    size_t id_len = strlen(id);
    size_t cap = 96 + id_len * 2 + payload_len;

    char *buf = (char *)alloc.alloc(alloc.ctx, cap);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, cap - pos,
        "{\"type\":\"res\",\"id\":\"");
    for (size_t i = 0; i < id_len && pos + 4 < cap; i++) {
        char c = id[i];
        if (c == '"' || c == '\\') { buf[pos++] = '\\'; buf[pos++] = c; }
        else buf[pos++] = c;
    }
    pos += (size_t)snprintf(buf + pos, cap - pos,
        "\",\"ok\":%s,\"payload\":%s}",
        ok ? "true" : "false", payload);

    sc_error_t err = sc_ws_server_send(conn, buf, pos);
    alloc.free(alloc.ctx, buf, cap);
    return err;
#else
    (void)ok;
    (void)payload_json;
    return SC_ERR_NOT_SUPPORTED;
#endif
}
