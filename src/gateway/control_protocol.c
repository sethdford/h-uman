#include "human/gateway/control_protocol.h"
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/security.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cp_internal.h"
#include "human/gateway/voice_stream.h"

#ifdef HU_GATEWAY_POSIX

typedef hu_error_t (*hu_rpc_handler_fn)(hu_allocator_t *alloc, hu_app_context_t *app,
                                        hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                        const hu_json_value_t *root, char **out, size_t *out_len);

typedef struct {
    const char *method;
    hu_rpc_handler_fn handler;
} hu_rpc_entry_t;

static const hu_rpc_entry_t s_rpc_table[] = {
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
#ifdef HU_HAS_CRON
    {"cron.list", cp_admin_cron_list},
    {"cron.add", cp_admin_cron_add},
    {"cron.remove", cp_admin_cron_remove},
    {"cron.run", cp_admin_cron_run},
    {"cron.update", cp_admin_cron_update},
    {"cron.runs", cp_admin_cron_runs},
#endif
#ifdef HU_HAS_SKILLS
    {"skills.list", cp_admin_skills_list},
    {"skills.enable", cp_admin_skills_enable},
    {"skills.disable", cp_admin_skills_disable},
    {"skills.install", cp_admin_skills_install},
    {"skills.search", cp_admin_skills_search},
    {"skills.uninstall", cp_admin_skills_uninstall},
    {"skills.update", cp_admin_skills_update},
#endif
    {"models.list", cp_admin_models_list},
    {"models.decisions", cp_admin_models_decisions},
    {"nodes.list", cp_admin_nodes_list},
    {"nodes.action", cp_admin_nodes_action},
    {"agents.list", cp_admin_agents_list},
#ifdef HU_HAS_UPDATE
    {"update.check", cp_admin_update_check},
    {"update.run", cp_admin_update_run},
#endif
    {"exec.approval.resolve", cp_admin_exec_approval},
    {"voice.transcribe", cp_voice_transcribe},
    {"voice.session.start", cp_voice_session_start},
    {"voice.session.stop", cp_voice_session_stop},
    {"voice.session.interrupt", cp_voice_session_interrupt},
    {"voice.audio.end", cp_voice_audio_end},
    {"voice.tool_response", cp_voice_tool_response},
    {"voice.config", cp_voice_config},
    {"voice.clone", cp_voice_clone},
    {"voice.validate", cp_voice_validate},
    {"usage.summary", cp_admin_usage_summary},
    {"metrics.snapshot", cp_admin_metrics_snapshot},
    {"metrics.fidelity", cp_admin_metrics_fidelity},
    {"metrics.directive_telemetry", cp_admin_metrics_directive_telemetry},
    {"activity.recent", cp_admin_activity_recent},
    {"memory.status", cp_memory_status},
    {"memory.list", cp_memory_list},
    {"memory.recall", cp_memory_recall},
    {"memory.store", cp_memory_store},
    {"memory.forget", cp_memory_forget},
    {"memory.ingest", cp_memory_ingest},
    {"memory.consolidate", cp_memory_consolidate},
    {"memory.graph", cp_memory_graph},
    {"hula.traces.list", cp_hula_traces_list},
    {"hula.traces.get", cp_hula_traces_get},
    {"hula.traces.delete", cp_hula_traces_delete},
    {"hula.traces.analytics", cp_hula_traces_analytics},
#ifdef HU_HAS_PUSH
    {"push.register", cp_admin_push_register},
    {"push.unregister", cp_admin_push_unregister},
#endif
    {"turing.scores", cp_turing_scores},
    {"turing.trend", cp_turing_trend},
    {"turing.dimensions", cp_turing_dimensions},
    {"turing.contact", cp_turing_contact},
    {"turing.trajectory", cp_turing_trajectory},
    {"turing.ab_tests", cp_turing_ab_tests},
    {"turing.channel", cp_turing_channel},
    {"security.cot.summary", cp_security_cot_summary},
    {"sota.metrics", cp_admin_metrics_snapshot},
    {"mcp.resources.list", cp_mcp_resources_list},
    {"mcp.prompts.list", cp_mcp_prompts_list},
    {"canvas.list", cp_canvas_list},
    {"canvas.get", cp_canvas_get},
    {"canvas.edit", cp_canvas_edit},
    {"canvas.undo", cp_canvas_undo},
    {"canvas.redo", cp_canvas_redo},
    {NULL, NULL},
};

/* ── Auth helpers ───────────────────────────────────────────────────── */

static bool is_public_method(const char *method) {
    return strcmp(method, "health") == 0 || strcmp(method, "connect") == 0 ||
           strcmp(method, "capabilities") == 0 || strcmp(method, "auth.oauth.start") == 0 ||
           strcmp(method, "auth.oauth.callback") == 0 || strcmp(method, "auth.oauth.refresh") == 0;
}

/* Admin-style RPC namespaces: dangerous when pairing is off and the socket is exposed. */
static bool is_sensitive_rpc_namespace(const char *method) {
    return strncmp(method, "memory.", 7) == 0 || strncmp(method, "config.", 7) == 0 ||
           strncmp(method, "cron.", 5) == 0 || strncmp(method, "skills.", 7) == 0 ||
           strncmp(method, "exec.", 5) == 0;
}

/* ── Method dispatcher ───────────────────────────────────────────────── */

static hu_error_t build_method_response(hu_allocator_t *alloc, const char *method,
                                        const hu_json_value_t *root, hu_app_context_t *app,
                                        hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                        char **payload_out, size_t *payload_len_out) {
    *payload_out = NULL;
    *payload_len_out = 0;

    for (const hu_rpc_entry_t *e = s_rpc_table; e->method; e++) {
        if (strcmp(method, e->method) == 0) {
            return e->handler(alloc, app, conn, proto, root, payload_out, payload_len_out);
        }
    }
    return HU_ERR_NOT_FOUND;
}

#endif /* HU_GATEWAY_POSIX */

/* ── Protocol lifecycle ──────────────────────────────────────────────── */

void hu_control_protocol_init(hu_control_protocol_t *proto, hu_allocator_t *alloc,
                              hu_ws_server_t *ws) {
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

void hu_control_protocol_deinit(hu_control_protocol_t *proto) {
    (void)proto;
}

void hu_control_set_app_ctx(hu_control_protocol_t *proto, hu_app_context_t *ctx) {
    if (proto)
        proto->app_ctx = ctx;
}

void hu_control_set_auth(hu_control_protocol_t *proto, bool require_pairing,
                         hu_pairing_guard_t *pairing_guard, const char *auth_token) {
    if (!proto)
        return;
    proto->require_pairing = require_pairing;
    proto->pairing_guard = pairing_guard;
    proto->auth_token = auth_token && auth_token[0] ? auth_token : NULL;
}

void hu_control_set_oauth(hu_control_protocol_t *proto, void *oauth_ctx) {
    if (!proto)
        return;
    proto->oauth_ctx = oauth_ctx;
}

void hu_control_set_oauth_pending(hu_control_protocol_t *proto, void *ctx,
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

void hu_control_on_message(hu_ws_conn_t *conn, const char *data, size_t data_len, void *ctx) {
    hu_control_protocol_t *proto = (hu_control_protocol_t *)ctx;
    if (!proto || !proto->alloc || !proto->ws)
        return;

#ifdef HU_GATEWAY_POSIX
    hu_json_value_t *root = NULL;
    hu_error_t parse_err = hu_json_parse(proto->alloc, data, data_len, &root);
    if (parse_err != HU_OK || !root) {
        hu_voice_stream_on_binary(proto, conn, data, data_len);
        return;
    }
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(proto->alloc, root);
        hu_voice_stream_on_binary(proto, conn, data, data_len);
        return;
    }

    const char *type_str = hu_json_get_string(root, "type");
    if (!type_str || strcmp(type_str, "req") != 0) {
        hu_json_free(proto->alloc, root);
        return;
    }

    /* Rate limit JSON-RPC requests only (not binary audio frames). */
    {
        uint64_t now_ms = (uint64_t)time(NULL) * 1000;
        if (now_ms - proto->rpc_window_ms > 1000) {
            proto->rpc_count = 0;
            proto->rpc_window_ms = now_ms;
        }
        if (++proto->rpc_count > 60) {
            hu_control_send_response(proto, conn, "0", false, "{\"error\":\"rate limited\"}");
            hu_json_free(proto->alloc, root);
            return;
        }
    }

    const char *id_raw = hu_json_get_string(root, "id");
    const char *method = hu_json_get_string(root, "method");

    if (!id_raw || !method) {
        hu_json_free(proto->alloc, root);
        return;
    }

    /* Auth check: when pairing required, only public methods and auth.token allowed unauthenticated
     */
    if (proto->require_pairing && !conn->authenticated && !is_public_method(method) &&
        strcmp(method, "auth.token") != 0) {
        hu_control_send_response(
            proto, conn, id_raw, false,
            "{\"error\":\"unauthorized\",\"message\":\"Authentication required\"}");
        hu_json_free(proto->alloc, root);
        return;
    }

    if (!proto->require_pairing && conn && !conn->authenticated &&
        is_sensitive_rpc_namespace(method)) {
        hu_log_warn("control", NULL, "blocked sensitive RPC '%s' - authentication required",
                    method);
        hu_control_send_response(proto, conn, id_raw, false,
                                 "{\"error\":\"unauthorized\",\"message\":\"Authentication "
                                 "required for sensitive operations\"}");
        hu_json_free(proto->alloc, root);
        return;
    }

    size_t id_slen = strlen(id_raw);
    char *id = (char *)proto->alloc->alloc(proto->alloc->ctx, id_slen + 1);
    if (!id) {
        hu_json_free(proto->alloc, root);
        return;
    }
    memcpy(id, id_raw, id_slen + 1);

    char *payload = NULL;
    size_t payload_len = 0;
    hu_error_t err = build_method_response(proto->alloc, method, root, proto->app_ctx, conn, proto,
                                           &payload, &payload_len);
    hu_json_free(proto->alloc, root);

    bool ok = (err == HU_OK);
    if (!payload && ok) {
        hu_json_value_t *empty = hu_json_object_new(proto->alloc);
        if (empty) {
            hu_json_stringify(proto->alloc, empty, &payload, &payload_len);
            hu_json_free(proto->alloc, empty);
        }
        if (!payload) {
            payload = (char *)proto->alloc->alloc(proto->alloc->ctx, 3);
            payload_len = 0;
        }
        if (payload && !payload_len) {
            memcpy(payload, "{}", 3);
            payload_len = 2;
        }
    } else if (err != HU_OK) {
        payload = (char *)proto->alloc->alloc(proto->alloc->ctx, 64);
        if (payload) {
            size_t n = 0;
            if (err == HU_ERR_NOT_FOUND) {
                n = (size_t)snprintf(payload, 64, "{\"error\":\"unknown method\"}");
            } else if (err == HU_ERR_GATEWAY_AUTH) {
                n = (size_t)snprintf(payload, 64, "{\"error\":\"invalid_token\"}");
            } else if (err == HU_ERR_INVALID_ARGUMENT) {
                n = (size_t)snprintf(payload, 64, "{\"error\":\"invalid_argument\"}");
            } else if (err == HU_ERR_NOT_SUPPORTED) {
                n = (size_t)snprintf(payload, 64, "{\"error\":\"not_supported\"}");
            } else {
                n = (size_t)snprintf(payload, 64, "{\"error\":\"internal\"}");
            }
            payload_len = n;
        }
    }

    if (payload) {
        if (payload_len > SIZE_MAX - 256) {
            if (err == HU_OK)
                proto->alloc->free(proto->alloc->ctx, payload, payload_len + 1);
            else
                proto->alloc->free(proto->alloc->ctx, payload, 64);
            proto->alloc->free(proto->alloc->ctx, id, id_slen + 1);
            return;
        }
        size_t res_cap = 256 + payload_len;
        char *res_buf = (char *)proto->alloc->alloc(proto->alloc->ctx, res_cap);
        if (res_buf) {
            size_t pos = 0;
            pos = hu_buf_appendf(res_buf, res_cap, pos, "{\"type\":\"res\",\"id\":\"");
            size_t id_len = strlen(id);
            char *id_esc = NULL;
            size_t esc_len = 0;
            if (id_len <= SIZE_MAX / 2 - 8U) {
                esc_len = id_len * 2 + 16;
                id_esc = (char *)proto->alloc->alloc(proto->alloc->ctx, esc_len);
            }
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
                pos = hu_buf_appendf(res_buf, res_cap, pos, "%s\"", id_esc);
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
            pos = hu_buf_appendf(res_buf, res_cap, pos, ",\"ok\":%s,\"payload\":",
                                 ok ? "true" : "false");
            if (pos + payload_len + 2 <= res_cap) {
                memcpy(res_buf + pos, payload, payload_len);
                pos += payload_len;
                res_buf[pos++] = '}';
                res_buf[pos] = '\0';
                hu_error_t send_err = hu_ws_server_send(proto->ws, conn, res_buf, pos);
                if (send_err != HU_OK) {
                    hu_log_error("control", NULL, "send response failed: %s",
                                 hu_error_string(send_err));
                }
                proto->alloc->free(proto->alloc->ctx, res_buf, res_cap);
            } else {
                proto->alloc->free(proto->alloc->ctx, res_buf, res_cap);
                (void)hu_control_send_response(proto, conn, id, false,
                                               "{\"error\":\"response_too_large\"}");
            }
        }
        if (err == HU_OK && payload) {
            proto->alloc->free(proto->alloc->ctx, payload, payload_len + 1);
        } else if (payload) {
            proto->alloc->free(proto->alloc->ctx, payload, 64);
        }
    }
    proto->alloc->free(proto->alloc->ctx, id, id_slen + 1);
#endif
}

void hu_control_on_close(hu_ws_conn_t *conn, void *ctx) {
    (void)ctx;
    hu_voice_stream_on_conn_close(conn);
}

/* ── Outbound helpers ────────────────────────────────────────────────── */

void hu_control_send_event(hu_control_protocol_t *proto, const char *event_name,
                           const char *payload_json) {
    if (!proto || !proto->ws || !event_name)
        return;

#ifdef HU_GATEWAY_POSIX
    uint64_t seq = proto->event_seq++;
    const char *payload = payload_json ? payload_json : "{}";
    size_t payload_len = strlen(payload);
    size_t event_len = strlen(event_name);

    if (event_len > SIZE_MAX - 128)
        return;
    if (payload_len > SIZE_MAX - 128 - event_len)
        return;
    size_t cap = 128 + event_len + payload_len;
    char *buf = (char *)proto->alloc->alloc(proto->alloc->ctx, cap);
    if (!buf)
        return;

    int n = snprintf(buf, cap, "{\"type\":\"event\",\"event\":\"%s\",\"payload\":%s,\"seq\":%llu}",
                     event_name, payload, (unsigned long long)seq);

    if (n > 0 && (size_t)n < cap)
        hu_ws_server_broadcast(proto->ws, buf, (size_t)n);

    proto->alloc->free(proto->alloc->ctx, buf, cap);
#endif
}

void hu_control_send_event_to_conn(hu_control_protocol_t *proto, hu_ws_conn_t *conn,
                                   const char *event_name, const char *payload_json) {
    if (!proto || !proto->ws || !conn || !event_name || !conn->active)
        return;

#ifdef HU_GATEWAY_POSIX
    uint64_t seq = proto->event_seq++;
    const char *payload = payload_json ? payload_json : "{}";
    size_t payload_len = strlen(payload);
    size_t event_len = strlen(event_name);

    if (event_len > SIZE_MAX - 128)
        return;
    if (payload_len > SIZE_MAX - 128 - event_len)
        return;
    size_t cap = 128 + event_len + payload_len;
    char *buf = (char *)proto->alloc->alloc(proto->alloc->ctx, cap);
    if (!buf)
        return;

    int n = snprintf(buf, cap, "{\"type\":\"event\",\"event\":\"%s\",\"payload\":%s,\"seq\":%llu}",
                     event_name, payload, (unsigned long long)seq);

    if (n > 0 && (size_t)n < cap)
        (void)hu_ws_server_send(proto->ws, conn, buf, (size_t)n);

    proto->alloc->free(proto->alloc->ctx, buf, cap);
#endif
}

hu_error_t hu_control_send_response(hu_control_protocol_t *proto, hu_ws_conn_t *conn,
                                    const char *id, bool ok, const char *payload_json) {
    if (!proto || !conn || !id)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_GATEWAY_POSIX
    hu_allocator_t *alloc = proto->alloc;
    if (!alloc || !proto->ws)
        return HU_ERR_INVALID_ARGUMENT;
    const char *payload = payload_json ? payload_json : "{}";
    size_t payload_len = strlen(payload);
    size_t id_len = strlen(id);
    if (id_len > SIZE_MAX / 2)
        return HU_ERR_OUT_OF_MEMORY;
    size_t id_esc_max = id_len * 2;
    if (id_esc_max > SIZE_MAX - 96)
        return HU_ERR_OUT_OF_MEMORY;
    size_t sum96 = 96 + id_esc_max;
    if (payload_len > SIZE_MAX - sum96)
        return HU_ERR_OUT_OF_MEMORY;
    size_t cap = sum96 + payload_len;

    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    pos = hu_buf_appendf(buf, cap, pos, "{\"type\":\"res\",\"id\":\"");
    for (size_t i = 0; i < id_len && pos + 4 < cap; i++) {
        char c = id[i];
        if (c == '"' || c == '\\') {
            buf[pos++] = '\\';
            buf[pos++] = c;
        } else
            buf[pos++] = c;
    }
    pos = hu_buf_appendf(buf, cap, pos, "\",\"ok\":%s,\"payload\":%s}",
                         ok ? "true" : "false", payload);

    hu_error_t err = hu_ws_server_send(proto->ws, conn, buf, pos);
    if (err != HU_OK) {
        hu_log_error("control", NULL, "send_response failed: %s", hu_error_string(err));
    }
    alloc->free(alloc->ctx, buf, cap);
    return err;
#else
    (void)ok;
    (void)payload_json;
    return HU_ERR_NOT_SUPPORTED;
#endif
}
