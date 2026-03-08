#include "seaclaw/gateway/control_protocol.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include "seaclaw/security.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cp_internal.h"

#ifdef SC_GATEWAY_POSIX

typedef sc_error_t (*sc_rpc_handler_fn)(sc_allocator_t *alloc, sc_app_context_t *app,
                                        sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                        const sc_json_value_t *root, char **out, size_t *out_len);

typedef struct {
    const char *method;
    sc_rpc_handler_fn handler;
} sc_rpc_entry_t;

static const sc_rpc_entry_t s_rpc_table[] = {
    {"auth.token", cp_admin_auth_token},
    {"auth.oauth.start", cp_admin_oauth_start},
    {"auth.oauth.callback", cp_admin_oauth_callback},
    {"auth.oauth.refresh", cp_admin_oauth_refresh},
    {"connect", cp_admin_connect},
    {"health", cp_admin_health},
    {"config.get", cp_config_get},
    {"config.schema", cp_config_schema},
    {"capabilities", cp_admin_capabilities},
    {"chat.send", cp_chat_send},
    {"chat.history", cp_chat_history},
    {"chat.abort", cp_chat_abort},
    {"config.set", cp_config_set},
    {"config.apply", cp_config_apply},
    {"sessions.list", cp_admin_sessions_list},
    {"sessions.patch", cp_admin_sessions_patch},
    {"sessions.delete", cp_admin_sessions_delete},
    {"persona.set", cp_admin_persona_set},
    {"tools.catalog", cp_admin_tools_catalog},
    {"channels.status", cp_admin_channels_status},
#ifdef SC_HAS_CRON
    {"cron.list", cp_admin_cron_list},
    {"cron.add", cp_admin_cron_add},
    {"cron.remove", cp_admin_cron_remove},
    {"cron.run", cp_admin_cron_run},
    {"cron.update", cp_admin_cron_update},
    {"cron.runs", cp_admin_cron_runs},
#endif
#ifdef SC_HAS_SKILLS
    {"skills.list", cp_admin_skills_list},
    {"skills.enable", cp_admin_skills_enable},
    {"skills.disable", cp_admin_skills_disable},
    {"skills.install", cp_admin_skills_install},
    {"skills.search", cp_admin_skills_search},
    {"skills.uninstall", cp_admin_skills_uninstall},
    {"skills.update", cp_admin_skills_update},
#endif
    {"models.list", cp_admin_models_list},
    {"nodes.list", cp_admin_nodes_list},
#ifdef SC_HAS_UPDATE
    {"update.check", cp_admin_update_check},
    {"update.run", cp_admin_update_run},
#endif
    {"exec.approval.resolve", cp_admin_exec_approval},
    {"usage.summary", cp_admin_usage_summary},
    {"metrics.snapshot", cp_admin_metrics_snapshot},
    {"activity.recent", cp_admin_activity_recent},
    {"memory.status", cp_memory_status},
    {"memory.list", cp_memory_list},
    {"memory.recall", cp_memory_recall},
    {"memory.store", cp_memory_store},
    {"memory.forget", cp_memory_forget},
    {"memory.ingest", cp_memory_ingest},
    {"memory.consolidate", cp_memory_consolidate},
#ifdef SC_HAS_PUSH
    {"push.register", cp_admin_push_register},
    {"push.unregister", cp_admin_push_unregister},
#endif
    {NULL, NULL},
};

/* ── Auth helpers ───────────────────────────────────────────────────── */

static bool is_public_method(const char *method) {
    return strcmp(method, "health") == 0 || strcmp(method, "status") == 0 ||
           strcmp(method, "version") == 0 || strcmp(method, "connect") == 0 ||
           strcmp(method, "capabilities") == 0 || strcmp(method, "auth.oauth.start") == 0 ||
           strcmp(method, "auth.oauth.callback") == 0 || strcmp(method, "auth.oauth.refresh") == 0;
}

/* ── Method dispatcher ───────────────────────────────────────────────── */

static sc_error_t build_method_response(sc_allocator_t *alloc, const char *method,
                                        const sc_json_value_t *root, sc_app_context_t *app,
                                        sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                        char **payload_out, size_t *payload_len_out) {
    *payload_out = NULL;
    *payload_len_out = 0;

    for (const sc_rpc_entry_t *e = s_rpc_table; e->method; e++) {
        if (strcmp(method, e->method) == 0) {
            return e->handler(alloc, app, conn, proto, root, payload_out, payload_len_out);
        }
    }
    return SC_ERR_NOT_FOUND;
}

#endif /* SC_GATEWAY_POSIX */

/* ── Protocol lifecycle ──────────────────────────────────────────────── */

void sc_control_protocol_init(sc_control_protocol_t *proto, sc_allocator_t *alloc,
                              sc_ws_server_t *ws) {
    if (!proto)
        return;
    proto->alloc = alloc;
    proto->ws = ws;
    proto->event_seq = 0;
    proto->app_ctx = NULL;
    proto->require_pairing = false;
    proto->pairing_guard = NULL;
    proto->auth_token = NULL;
    proto->oauth_ctx = NULL;
    proto->oauth_pending_ctx = NULL;
    proto->oauth_pending_store = NULL;
    proto->oauth_pending_lookup = NULL;
    proto->oauth_pending_remove = NULL;
}

void sc_control_protocol_deinit(sc_control_protocol_t *proto) {
    (void)proto;
}

void sc_control_set_app_ctx(sc_control_protocol_t *proto, sc_app_context_t *ctx) {
    if (proto)
        proto->app_ctx = ctx;
}

void sc_control_set_auth(sc_control_protocol_t *proto, bool require_pairing,
                         sc_pairing_guard_t *pairing_guard, const char *auth_token) {
    if (!proto)
        return;
    proto->require_pairing = require_pairing;
    proto->pairing_guard = pairing_guard;
    proto->auth_token = auth_token && auth_token[0] ? auth_token : NULL;
}

void sc_control_set_oauth(sc_control_protocol_t *proto, void *oauth_ctx) {
    if (!proto)
        return;
    proto->oauth_ctx = oauth_ctx;
}

void sc_control_set_oauth_pending(sc_control_protocol_t *proto, void *ctx,
                                  void (*store)(void *ctx, const char *state, const char *verifier),
                                  const char *(*lookup)(void *ctx, const char *state),
                                  void (*remove)(void *ctx, const char *state)) {
    if (!proto)
        return;
    proto->oauth_pending_ctx = ctx;
    proto->oauth_pending_store = store;
    proto->oauth_pending_lookup = lookup;
    proto->oauth_pending_remove = remove;
}

/* ── Incoming message handler ────────────────────────────────────────── */

void sc_control_on_message(sc_ws_conn_t *conn, const char *data, size_t data_len, void *ctx) {
    sc_control_protocol_t *proto = (sc_control_protocol_t *)ctx;
    if (!proto || !proto->alloc || !proto->ws)
        return;

#ifdef SC_GATEWAY_POSIX
    /* Simple global RPC rate limit: max 60 calls per second across all connections */
    uint64_t now_ms = (uint64_t)time(NULL) * 1000;
    if (now_ms - proto->rpc_window_ms > 1000) {
        proto->rpc_count = 0;
        proto->rpc_window_ms = now_ms;
    }
    if (++proto->rpc_count > 60) {
        sc_control_send_response(conn, "0", false, "{\"error\":\"rate limited\"}");
        return;
    }

    sc_json_value_t *root = NULL;
    if (sc_json_parse(proto->alloc, data, data_len, &root) != SC_OK)
        return;
    if (!root || root->type != SC_JSON_OBJECT) {
        if (root)
            sc_json_free(proto->alloc, root);
        return;
    }

    const char *type_str = sc_json_get_string(root, "type");
    if (!type_str || strcmp(type_str, "req") != 0) {
        sc_json_free(proto->alloc, root);
        return;
    }

    const char *id_raw = sc_json_get_string(root, "id");
    const char *method = sc_json_get_string(root, "method");

    if (!id_raw || !method) {
        sc_json_free(proto->alloc, root);
        return;
    }

    /* Auth check: when pairing required, only public methods and auth.token allowed unauthenticated
     */
    if (proto->require_pairing && !conn->authenticated && !is_public_method(method) &&
        strcmp(method, "auth.token") != 0) {
        sc_control_send_response(
            conn, id_raw, false,
            "{\"error\":\"unauthorized\",\"message\":\"Authentication required\"}");
        sc_json_free(proto->alloc, root);
        return;
    }

    size_t id_slen = strlen(id_raw);
    char *id = (char *)proto->alloc->alloc(proto->alloc->ctx, id_slen + 1);
    if (!id) {
        sc_json_free(proto->alloc, root);
        return;
    }
    memcpy(id, id_raw, id_slen + 1);

    char *payload = NULL;
    size_t payload_len = 0;
    sc_error_t err = build_method_response(proto->alloc, method, root, proto->app_ctx, conn, proto,
                                           &payload, &payload_len);
    sc_json_free(proto->alloc, root);

    bool ok = (err == SC_OK);
    if (!payload && ok) {
        sc_json_value_t *empty = sc_json_object_new(proto->alloc);
        if (empty) {
            sc_json_stringify(proto->alloc, empty, &payload, &payload_len);
            sc_json_free(proto->alloc, empty);
        }
        if (!payload)
            payload = (char *)proto->alloc->alloc(proto->alloc->ctx, 3);
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
            } else if (err == SC_ERR_GATEWAY_AUTH) {
                n = (size_t)snprintf(payload, 64, "{\"error\":\"invalid_token\"}");
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
            pos += (size_t)snprintf(res_buf, res_cap, "{\"type\":\"res\",\"id\":\"");
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
            } else {
                for (size_t i = 0; i < id_len && pos + 4 < res_cap; i++) {
                    char c = id[i];
                    if (c == '"' || c == '\\')
                        res_buf[pos++] = '\\';
                    res_buf[pos++] = c;
                }
                if (pos < res_cap)
                    res_buf[pos++] = '"';
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
        if (err == SC_OK && payload) {
            proto->alloc->free(proto->alloc->ctx, payload, payload_len + 1);
        } else if (payload) {
            proto->alloc->free(proto->alloc->ctx, payload, 64);
        }
    }
    proto->alloc->free(proto->alloc->ctx, id, id_slen + 1);
#endif
}

void sc_control_on_close(sc_ws_conn_t *conn, void *ctx) {
    (void)conn;
    (void)ctx;
}

/* ── Outbound helpers ────────────────────────────────────────────────── */

void sc_control_send_event(sc_control_protocol_t *proto, const char *event_name,
                           const char *payload_json) {
    if (!proto || !proto->ws || !event_name)
        return;

#ifdef SC_GATEWAY_POSIX
    uint64_t seq = proto->event_seq++;
    const char *payload = payload_json ? payload_json : "{}";
    size_t payload_len = strlen(payload);
    size_t event_len = strlen(event_name);

    size_t cap = 128 + event_len + payload_len;
    char *buf = (char *)proto->alloc->alloc(proto->alloc->ctx, cap);
    if (!buf)
        return;

    int n = snprintf(buf, cap, "{\"type\":\"event\",\"event\":\"%s\",\"payload\":%s,\"seq\":%llu}",
                     event_name, payload, (unsigned long long)seq);

    if (n > 0 && (size_t)n < cap)
        sc_ws_server_broadcast(proto->ws, buf, (size_t)n);

    proto->alloc->free(proto->alloc->ctx, buf, cap);
#endif
}

sc_error_t sc_control_send_response(sc_ws_conn_t *conn, const char *id, bool ok,
                                    const char *payload_json) {
    if (!conn || !id)
        return SC_ERR_INVALID_ARGUMENT;

#ifdef SC_GATEWAY_POSIX
    sc_allocator_t alloc = sc_system_allocator();
    const char *payload = payload_json ? payload_json : "{}";
    size_t payload_len = strlen(payload);
    size_t id_len = strlen(id);
    size_t cap = 96 + id_len * 2 + payload_len;

    char *buf = (char *)alloc.alloc(alloc.ctx, cap);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, cap - pos, "{\"type\":\"res\",\"id\":\"");
    for (size_t i = 0; i < id_len && pos + 4 < cap; i++) {
        char c = id[i];
        if (c == '"' || c == '\\') {
            buf[pos++] = '\\';
            buf[pos++] = c;
        } else
            buf[pos++] = c;
    }
    pos += (size_t)snprintf(buf + pos, cap - pos, "\",\"ok\":%s,\"payload\":%s}",
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
