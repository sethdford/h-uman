#include "seaclaw/security/audit.h"
#include <stdint.h>
#include "seaclaw/core/error.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

#define SC_AUDIT_JSON_BUF_SIZE 4096
#define SC_AUDIT_MAX_PATH 1024

static uint64_t g_audit_next_id = 0;

const char *sc_audit_event_type_string(sc_audit_event_type_t t) {
    switch (t) {
        case SC_AUDIT_COMMAND_EXECUTION: return "command_execution";
        case SC_AUDIT_FILE_ACCESS:       return "file_access";
        case SC_AUDIT_CONFIG_CHANGE:     return "config_change";
        case SC_AUDIT_AUTH_SUCCESS:      return "auth_success";
        case SC_AUDIT_AUTH_FAILURE:      return "auth_failure";
        case SC_AUDIT_POLICY_VIOLATION:  return "policy_violation";
        case SC_AUDIT_SECURITY_EVENT:    return "security_event";
        default:                         return "unknown";
    }
}

sc_audit_severity_t sc_audit_event_severity(sc_audit_event_type_t t) {
    switch (t) {
        case SC_AUDIT_AUTH_FAILURE:
        case SC_AUDIT_POLICY_VIOLATION:
            return SC_AUDIT_SEV_HIGH;
        case SC_AUDIT_COMMAND_EXECUTION:
        case SC_AUDIT_FILE_ACCESS:
        case SC_AUDIT_CONFIG_CHANGE:
            return SC_AUDIT_SEV_MEDIUM;
        default:
            return SC_AUDIT_SEV_LOW;
    }
}

bool sc_audit_should_log(sc_audit_event_type_t type, sc_audit_severity_t min_sev) {
    return sc_audit_event_severity(type) >= min_sev;
}

static size_t escape_json_string(const char *s, char *out, size_t out_cap) {
    if (!s || out_cap == 0) return 0;
    size_t n = 0;
    for (; *s && n + 2 < out_cap; s++) {
        if (*s == '"' || *s == '\\') {
            out[n++] = '\\';
            out[n++] = *s;
        } else if ((unsigned char)*s < 32) {
            n += (size_t)snprintf(out + n, out_cap - n, "\\u%04x", (unsigned char)*s);
        } else {
            out[n++] = *s;
        }
    }
    return n;
}

void sc_audit_event_init(sc_audit_event_t *ev, sc_audit_event_type_t type) {
    if (!ev) return;
    ev->timestamp_s = (int64_t)time(NULL);
    ev->event_id = __sync_add_and_fetch(&g_audit_next_id, 1);
    ev->event_type = type;
    ev->actor.channel = NULL;
    ev->actor.user_id = NULL;
    ev->actor.username = NULL;
    ev->action.command = NULL;
    ev->action.risk_level = NULL;
    ev->action.approved = false;
    ev->action.allowed = false;
    ev->result.success = false;
    ev->result.exit_code = -1;
    ev->result.duration_ms = 0;
    ev->result.err_msg = NULL;
    ev->security.policy_violation = false;
    ev->security.rate_limit_remaining = 0;
    ev->security.sandbox_backend = NULL;
}

void sc_audit_event_with_actor(sc_audit_event_t *ev,
    const char *channel, const char *user_id, const char *username) {
    if (!ev) return;
    ev->actor.channel = channel;
    ev->actor.user_id = user_id;
    ev->actor.username = username;
}

void sc_audit_event_with_action(sc_audit_event_t *ev,
    const char *command, const char *risk_level, bool approved, bool allowed) {
    if (!ev) return;
    ev->action.command = command;
    ev->action.risk_level = risk_level;
    ev->action.approved = approved;
    ev->action.allowed = allowed;
}

void sc_audit_event_with_result(sc_audit_event_t *ev,
    bool success, int32_t exit_code, uint64_t duration_ms, const char *err_msg) {
    if (!ev) return;
    ev->result.success = success;
    ev->result.exit_code = exit_code;
    ev->result.duration_ms = duration_ms;
    ev->result.err_msg = err_msg;
}

void sc_audit_event_with_security(sc_audit_event_t *ev,
    const char *sandbox_backend) {
    if (!ev) return;
    ev->security.sandbox_backend = sandbox_backend;
}

size_t sc_audit_event_write_json(const sc_audit_event_t *ev,
    char *buf, size_t buf_size) {
    if (!ev || !buf || buf_size < 64) return 0;

    int n = snprintf(buf, buf_size,
        "{\"timestamp_s\":%ld,\"event_id\":%lu,\"event_type\":\"%s\"",
        (long)ev->timestamp_s,
        (unsigned long)ev->event_id,
        sc_audit_event_type_string(ev->event_type));
    if (n < 0 || (size_t)n >= buf_size) return 0;
    size_t pos = (size_t)n;

    if (ev->actor.channel) {
        char esc[256];
        size_t elen = escape_json_string(ev->actor.channel, esc, sizeof(esc));
        esc[elen] = '\0';
        n = snprintf(buf + pos, buf_size - pos,
            ",\"actor\":{\"channel\":\"%s\"", esc);
        if (n < 0 || (size_t)n >= buf_size - pos) return pos;
        pos += (size_t)n;
        if (ev->actor.user_id) {
            elen = escape_json_string(ev->actor.user_id, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, ",\"user_id\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
        }
        if (ev->actor.username) {
            elen = escape_json_string(ev->actor.username, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, ",\"username\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
        }
        n = snprintf(buf + pos, buf_size - pos, "}");
        if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
    }

    if (ev->action.command || ev->action.risk_level ||
        ev->action.approved || ev->action.allowed) {
        n = snprintf(buf + pos, buf_size - pos, ",\"action\":{");
        if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
        bool need_comma = false;
        if (ev->action.command) {
            char esc[512];
            size_t elen = escape_json_string(ev->action.command, esc, sizeof(esc));
            if (elen >= sizeof(esc)) elen = sizeof(esc) - 1;
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, "\"command\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos) { pos += (size_t)n; need_comma = true; }
        }
        if (ev->action.risk_level) {
            char esc[64];
            size_t elen = escape_json_string(ev->action.risk_level, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, "%s\"risk_level\":\"%s\"",
                need_comma ? "," : "", esc);
            if (n >= 0 && (size_t)n < buf_size - pos) { pos += (size_t)n; need_comma = true; }
        }
        n = snprintf(buf + pos, buf_size - pos, "%s\"approved\":%s,\"allowed\":%s}",
            need_comma ? "," : "",
            ev->action.approved ? "true" : "false",
            ev->action.allowed ? "true" : "false");
        if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
    }

    if (ev->result.duration_ms > 0 || ev->result.err_msg) {
        n = snprintf(buf + pos, buf_size - pos,
            ",\"result\":{\"success\":%s",
            ev->result.success ? "true" : "false");
        if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
        if (ev->result.exit_code >= 0) {
            n = snprintf(buf + pos, buf_size - pos, ",\"exit_code\":%d", ev->result.exit_code);
            if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
        }
        if (ev->result.duration_ms > 0) {
            n = snprintf(buf + pos, buf_size - pos, ",\"duration_ms\":%lu",
                (unsigned long)ev->result.duration_ms);
            if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
        }
        if (ev->result.err_msg) {
            char esc[256];
            size_t elen = escape_json_string(ev->result.err_msg, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, ",\"error\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
        }
        n = snprintf(buf + pos, buf_size - pos, "}");
        if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
    }

    n = snprintf(buf + pos, buf_size - pos,
        ",\"security\":{\"policy_violation\":%s",
        ev->security.policy_violation ? "true" : "false");
    if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
    if (ev->security.rate_limit_remaining > 0) {
        n = snprintf(buf + pos, buf_size - pos, ",\"rate_limit_remaining\":%u",
            ev->security.rate_limit_remaining);
        if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
    }
    if (ev->security.sandbox_backend) {
        char esc[64];
        size_t elen = escape_json_string(ev->security.sandbox_backend, esc, sizeof(esc));
        esc[elen] = '\0';
        n = snprintf(buf + pos, buf_size - pos, ",\"sandbox_backend\":\"%s\"", esc);
        if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
    }
    n = snprintf(buf + pos, buf_size - pos, "}}");
    if (n >= 0 && (size_t)n < buf_size - pos) pos += (size_t)n;

    return pos;
}

struct sc_audit_logger {
    char *log_path;
    sc_audit_config_t config;
    sc_allocator_t *alloc;
};

sc_audit_logger_t *sc_audit_logger_create(sc_allocator_t *alloc,
    const sc_audit_config_t *config, const char *base_dir) {
    if (!alloc || !config || !base_dir) return NULL;

    size_t base_len = strlen(base_dir);
    size_t path_len = strlen(config->log_path);
    if (base_len + path_len + 3 > SC_AUDIT_MAX_PATH) return NULL;

    char path[SC_AUDIT_MAX_PATH];
    if (base_len > 0 && base_dir[base_len - 1] == '/')
        snprintf(path, sizeof(path), "%s%s", base_dir, config->log_path);
    else
        snprintf(path, sizeof(path), "%s/%s", base_dir, config->log_path);

    sc_audit_logger_t *logger = (sc_audit_logger_t *)alloc->alloc(alloc->ctx,
        sizeof(sc_audit_logger_t));
    if (!logger) return NULL;

    logger->log_path = (char *)alloc->alloc(alloc->ctx, strlen(path) + 1);
    if (!logger->log_path) {
        alloc->free(alloc->ctx, logger, sizeof(sc_audit_logger_t));
        return NULL;
    }
    strcpy(logger->log_path, path);
    logger->config = *config;
    logger->alloc = alloc;
    return logger;
}

void sc_audit_logger_destroy(sc_audit_logger_t *logger, sc_allocator_t *alloc) {
    if (!logger || !alloc) return;
    if (logger->log_path) {
        size_t len = strlen(logger->log_path) + 1;
        alloc->free(alloc->ctx, logger->log_path, len);
    }
    alloc->free(alloc->ctx, logger, sizeof(sc_audit_logger_t));
}

sc_error_t sc_audit_logger_log(sc_audit_logger_t *logger,
    const sc_audit_event_t *event) {
    if (!logger || !event) return SC_ERR_INVALID_ARGUMENT;
    if (!logger->config.enabled) return SC_OK;

    char json_buf[SC_AUDIT_JSON_BUF_SIZE];
    size_t json_len = sc_audit_event_write_json(event, json_buf, sizeof(json_buf));
    if (json_len == 0) return SC_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(logger->log_path, "a");
    if (!f) return SC_ERR_IO;

    size_t written = fwrite(json_buf, 1, json_len, f);
    if (written != json_len) {
        fclose(f);
        return SC_ERR_IO;
    }
    if (fputc('\n', f) == EOF) {
        fclose(f);
        return SC_ERR_IO;
    }
    fflush(f);
    fclose(f);
    return SC_OK;
}

sc_error_t sc_audit_logger_log_command(sc_audit_logger_t *logger,
    const sc_audit_cmd_log_t *entry) {
    if (!logger || !entry) return SC_ERR_INVALID_ARGUMENT;

    sc_audit_event_t ev;
    sc_audit_event_init(&ev, SC_AUDIT_COMMAND_EXECUTION);
    sc_audit_event_with_actor(&ev, entry->channel, NULL, NULL);
    sc_audit_event_with_action(&ev,
        entry->command, entry->risk_level,
        entry->approved, entry->allowed);
    sc_audit_event_with_result(&ev, entry->success, -1, entry->duration_ms, NULL);

    return sc_audit_logger_log(logger, &ev);
}
