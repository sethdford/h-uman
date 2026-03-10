#include "human/security/audit.h"
#include "human/core/error.h"
#include "human/crypto.h"
#include "human/security.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define HU_AUDIT_JSON_BUF_SIZE 4096
#define HU_AUDIT_MAX_PATH      1024
#define HU_AUDIT_HMAC_LEN      32

static void build_key_path(char *out, size_t cap, const char *base, size_t blen);

static _Atomic uint64_t g_audit_next_id = 0;

static void audit_secure_zero(void *p, size_t n) {
#if defined(__STDC_LIB_EXT1__)
    memset_s(p, n, 0, n);
#elif defined(__GNUC__) || defined(__clang__)
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");
#else
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--)
        *vp++ = 0;
#endif
}

const char *hu_audit_event_type_string(hu_audit_event_type_t t) {
    switch (t) {
    case HU_AUDIT_COMMAND_EXECUTION:
        return "command_execution";
    case HU_AUDIT_FILE_ACCESS:
        return "file_access";
    case HU_AUDIT_CONFIG_CHANGE:
        return "config_change";
    case HU_AUDIT_AUTH_SUCCESS:
        return "auth_success";
    case HU_AUDIT_AUTH_FAILURE:
        return "auth_failure";
    case HU_AUDIT_POLICY_VIOLATION:
        return "policy_violation";
    case HU_AUDIT_SECURITY_EVENT:
        return "security_event";
    default:
        return "unknown";
    }
}

hu_audit_severity_t hu_audit_event_severity(hu_audit_event_type_t t) {
    switch (t) {
    case HU_AUDIT_AUTH_FAILURE:
    case HU_AUDIT_POLICY_VIOLATION:
        return HU_AUDIT_SEV_HIGH;
    case HU_AUDIT_COMMAND_EXECUTION:
    case HU_AUDIT_FILE_ACCESS:
    case HU_AUDIT_CONFIG_CHANGE:
        return HU_AUDIT_SEV_MEDIUM;
    default:
        return HU_AUDIT_SEV_LOW;
    }
}

bool hu_audit_should_log(hu_audit_event_type_t type, hu_audit_severity_t min_sev) {
    return hu_audit_event_severity(type) >= min_sev;
}

static size_t escape_json_string(const char *s, char *out, size_t out_cap) {
    if (!s || out_cap == 0)
        return 0;
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

void hu_audit_event_init(hu_audit_event_t *ev, hu_audit_event_type_t type) {
    if (!ev)
        return;
    ev->timestamp_s = (int64_t)time(NULL);
    ev->event_id = atomic_fetch_add_explicit(&g_audit_next_id, 1, memory_order_relaxed) + 1;
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
    ev->identity.agent_id = 0;
    ev->identity.model_version = NULL;
    ev->identity.auth_token_hash = NULL;
    ev->input.trigger_type = NULL;
    ev->input.trigger_source = NULL;
    ev->input.prompt_hash = NULL;
    ev->input.prompt_length = 0;
    ev->reasoning.decision = NULL;
    ev->reasoning.rule_name = NULL;
    ev->reasoning.confidence = -1.0f;
    ev->reasoning.context_tokens = 0;
}

void hu_audit_event_with_actor(hu_audit_event_t *ev, const char *channel, const char *user_id,
                               const char *username) {
    if (!ev)
        return;
    ev->actor.channel = channel;
    ev->actor.user_id = user_id;
    ev->actor.username = username;
}

void hu_audit_event_with_action(hu_audit_event_t *ev, const char *command, const char *risk_level,
                                bool approved, bool allowed) {
    if (!ev)
        return;
    ev->action.command = command;
    ev->action.risk_level = risk_level;
    ev->action.approved = approved;
    ev->action.allowed = allowed;
}

void hu_audit_event_with_result(hu_audit_event_t *ev, bool success, int32_t exit_code,
                                uint64_t duration_ms, const char *err_msg) {
    if (!ev)
        return;
    ev->result.success = success;
    ev->result.exit_code = exit_code;
    ev->result.duration_ms = duration_ms;
    ev->result.err_msg = err_msg;
}

void hu_audit_event_with_security(hu_audit_event_t *ev, const char *sandbox_backend) {
    if (!ev)
        return;
    ev->security.sandbox_backend = sandbox_backend;
}

void hu_audit_event_with_identity(hu_audit_event_t *ev, uint64_t agent_id,
                                  const char *model_version, const char *auth_token_hash) {
    if (!ev)
        return;
    ev->identity.agent_id = agent_id;
    ev->identity.model_version = model_version;
    ev->identity.auth_token_hash = auth_token_hash;
}

void hu_audit_event_with_input(hu_audit_event_t *ev, const char *trigger_type,
                               const char *trigger_source, const char *prompt_hash,
                               size_t prompt_length) {
    if (!ev)
        return;
    ev->input.trigger_type = trigger_type;
    ev->input.trigger_source = trigger_source;
    ev->input.prompt_hash = prompt_hash;
    ev->input.prompt_length = prompt_length;
}

void hu_audit_event_with_reasoning(hu_audit_event_t *ev, const char *decision,
                                   const char *rule_name, float confidence,
                                   uint32_t context_tokens) {
    if (!ev)
        return;
    ev->reasoning.decision = decision;
    ev->reasoning.rule_name = rule_name;
    ev->reasoning.confidence = confidence;
    ev->reasoning.context_tokens = context_tokens;
}

size_t hu_audit_event_write_json(const hu_audit_event_t *ev, char *buf, size_t buf_size) {
    if (!ev || !buf || buf_size < 64)
        return 0;

    int n = snprintf(buf, buf_size, "{\"timestamp_s\":%ld,\"event_id\":%lu,\"event_type\":\"%s\"",
                     (long)ev->timestamp_s, (unsigned long)ev->event_id,
                     hu_audit_event_type_string(ev->event_type));
    if (n < 0 || (size_t)n >= buf_size)
        return 0;
    size_t pos = (size_t)n;

    /* identity: agent_id, model_version, auth_token_hash */
    if (ev->identity.agent_id > 0 || ev->identity.model_version || ev->identity.auth_token_hash) {
        n = snprintf(buf + pos, buf_size - pos, ",\"identity\":{\"agent_id\":%lu",
                     (unsigned long)ev->identity.agent_id);
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
        if (ev->identity.model_version) {
            char esc[128];
            size_t elen = escape_json_string(ev->identity.model_version, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, ",\"model_version\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos)
                pos += (size_t)n;
        }
        if (ev->identity.auth_token_hash) {
            char esc[32];
            size_t elen = escape_json_string(ev->identity.auth_token_hash, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, ",\"auth_token_hash\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos)
                pos += (size_t)n;
        }
        n = snprintf(buf + pos, buf_size - pos, "}");
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
    }

    /* input: trigger_type, trigger_source, prompt_hash, prompt_length */
    if (ev->input.trigger_type || ev->input.trigger_source || ev->input.prompt_hash ||
        ev->input.prompt_length > 0) {
        n = snprintf(buf + pos, buf_size - pos, ",\"input\":{");
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
        bool need_comma = false;
        if (ev->input.trigger_type) {
            char esc[64];
            size_t elen = escape_json_string(ev->input.trigger_type, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, "\"trigger_type\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos) {
                pos += (size_t)n;
                need_comma = true;
            }
        }
        if (ev->input.trigger_source) {
            char esc[64];
            size_t elen = escape_json_string(ev->input.trigger_source, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, "%s\"trigger_source\":\"%s\"",
                         need_comma ? "," : "", esc);
            if (n >= 0 && (size_t)n < buf_size - pos) {
                pos += (size_t)n;
                need_comma = true;
            }
        }
        if (ev->input.prompt_hash) {
            char esc[80];
            size_t elen = escape_json_string(ev->input.prompt_hash, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, "%s\"prompt_hash\":\"%s\"",
                         need_comma ? "," : "", esc);
            if (n >= 0 && (size_t)n < buf_size - pos) {
                pos += (size_t)n;
                need_comma = true;
            }
        }
        if (ev->input.prompt_length > 0) {
            n = snprintf(buf + pos, buf_size - pos, "%s\"prompt_length\":%zu",
                         need_comma ? "," : "", ev->input.prompt_length);
            if (n >= 0 && (size_t)n < buf_size - pos)
                pos += (size_t)n;
        }
        n = snprintf(buf + pos, buf_size - pos, "}");
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
    }

    /* reasoning: decision, rule_name, confidence, context_tokens */
    if (ev->reasoning.decision || ev->reasoning.rule_name || ev->reasoning.confidence >= 0.0f ||
        ev->reasoning.context_tokens > 0) {
        n = snprintf(buf + pos, buf_size - pos, ",\"reasoning\":{");
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
        bool need_comma = false;
        if (ev->reasoning.decision) {
            char esc[64];
            size_t elen = escape_json_string(ev->reasoning.decision, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, "\"decision\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos) {
                pos += (size_t)n;
                need_comma = true;
            }
        }
        if (ev->reasoning.rule_name) {
            char esc[64];
            size_t elen = escape_json_string(ev->reasoning.rule_name, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, "%s\"rule_name\":\"%s\"", need_comma ? "," : "",
                         esc);
            if (n >= 0 && (size_t)n < buf_size - pos) {
                pos += (size_t)n;
                need_comma = true;
            }
        }
        if (ev->reasoning.confidence >= 0.0f) {
            n = snprintf(buf + pos, buf_size - pos, "%s\"confidence\":%.2f", need_comma ? "," : "",
                         (double)ev->reasoning.confidence);
            if (n >= 0 && (size_t)n < buf_size - pos) {
                pos += (size_t)n;
                need_comma = true;
            }
        }
        if (ev->reasoning.context_tokens > 0) {
            n = snprintf(buf + pos, buf_size - pos, "%s\"context_tokens\":%u",
                         need_comma ? "," : "", ev->reasoning.context_tokens);
            if (n >= 0 && (size_t)n < buf_size - pos)
                pos += (size_t)n;
        }
        n = snprintf(buf + pos, buf_size - pos, "}");
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
    }

    if (ev->actor.channel) {
        char esc[256];
        size_t elen = escape_json_string(ev->actor.channel, esc, sizeof(esc));
        esc[elen] = '\0';
        n = snprintf(buf + pos, buf_size - pos, ",\"actor\":{\"channel\":\"%s\"", esc);
        if (n < 0 || (size_t)n >= buf_size - pos)
            return pos;
        pos += (size_t)n;
        if (ev->actor.user_id) {
            elen = escape_json_string(ev->actor.user_id, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, ",\"user_id\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos)
                pos += (size_t)n;
        }
        if (ev->actor.username) {
            elen = escape_json_string(ev->actor.username, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, ",\"username\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos)
                pos += (size_t)n;
        }
        n = snprintf(buf + pos, buf_size - pos, "}");
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
    }

    if (ev->action.command || ev->action.risk_level || ev->action.approved || ev->action.allowed) {
        n = snprintf(buf + pos, buf_size - pos, ",\"action\":{");
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
        bool need_comma = false;
        if (ev->action.command) {
            /* Before writing command to audit log, truncate long commands */
            char safe_cmd[512];
            size_t cmd_len = strlen(ev->action.command);
            if (cmd_len > 500)
                cmd_len = 500;
            memcpy(safe_cmd, ev->action.command, cmd_len);
            safe_cmd[cmd_len] = '\0';
            char esc[512];
            size_t elen = escape_json_string(safe_cmd, esc, sizeof(esc));
            if (elen >= sizeof(esc))
                elen = sizeof(esc) - 1;
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, "\"command\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos) {
                pos += (size_t)n;
                need_comma = true;
            }
        }
        if (ev->action.risk_level) {
            char esc[64];
            size_t elen = escape_json_string(ev->action.risk_level, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, "%s\"risk_level\":\"%s\"",
                         need_comma ? "," : "", esc);
            if (n >= 0 && (size_t)n < buf_size - pos) {
                pos += (size_t)n;
                need_comma = true;
            }
        }
        n = snprintf(buf + pos, buf_size - pos, "%s\"approved\":%s,\"allowed\":%s}",
                     need_comma ? "," : "", ev->action.approved ? "true" : "false",
                     ev->action.allowed ? "true" : "false");
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
    }

    if (ev->result.duration_ms > 0 || ev->result.err_msg) {
        n = snprintf(buf + pos, buf_size - pos, ",\"result\":{\"success\":%s",
                     ev->result.success ? "true" : "false");
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
        if (ev->result.exit_code >= 0) {
            n = snprintf(buf + pos, buf_size - pos, ",\"exit_code\":%d", ev->result.exit_code);
            if (n >= 0 && (size_t)n < buf_size - pos)
                pos += (size_t)n;
        }
        if (ev->result.duration_ms > 0) {
            n = snprintf(buf + pos, buf_size - pos, ",\"duration_ms\":%lu",
                         (unsigned long)ev->result.duration_ms);
            if (n >= 0 && (size_t)n < buf_size - pos)
                pos += (size_t)n;
        }
        if (ev->result.err_msg) {
            char esc[256];
            size_t elen = escape_json_string(ev->result.err_msg, esc, sizeof(esc));
            esc[elen] = '\0';
            n = snprintf(buf + pos, buf_size - pos, ",\"error\":\"%s\"", esc);
            if (n >= 0 && (size_t)n < buf_size - pos)
                pos += (size_t)n;
        }
        n = snprintf(buf + pos, buf_size - pos, "}");
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
    }

    n = snprintf(buf + pos, buf_size - pos, ",\"security\":{\"policy_violation\":%s",
                 ev->security.policy_violation ? "true" : "false");
    if (n >= 0 && (size_t)n < buf_size - pos)
        pos += (size_t)n;
    if (ev->security.rate_limit_remaining > 0) {
        n = snprintf(buf + pos, buf_size - pos, ",\"rate_limit_remaining\":%u",
                     ev->security.rate_limit_remaining);
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
    }
    if (ev->security.sandbox_backend) {
        char esc[64];
        size_t elen = escape_json_string(ev->security.sandbox_backend, esc, sizeof(esc));
        esc[elen] = '\0';
        n = snprintf(buf + pos, buf_size - pos, ",\"sandbox_backend\":\"%s\"", esc);
        if (n >= 0 && (size_t)n < buf_size - pos)
            pos += (size_t)n;
    }
    n = snprintf(buf + pos, buf_size - pos, "}}");
    if (n >= 0 && (size_t)n < buf_size - pos)
        pos += (size_t)n;

    return pos;
}

#define HU_AUDIT_MAX_KEY_HISTORY  64
#define HU_AUDIT_KEY_HISTORY_LINE 128

struct hu_audit_logger {
    char *log_path;
    char *key_path;
    unsigned char prev_hmac[HU_AUDIT_HMAC_LEN];
    unsigned char audit_key[HU_AUDIT_HMAC_LEN];
    bool chain_initialized;
    hu_audit_config_t config;
    hu_allocator_t *alloc;
    uint32_t rotation_interval_hours;
    time_t last_rotation_time;
};

static void get_key_history_path(const char *key_path, char *out, size_t out_cap) {
    const char *slash = strrchr(key_path, '/');
    if (slash && slash > key_path) {
        size_t dlen = (size_t)(slash - key_path);
        if (dlen + 24 < out_cap) {
            memcpy(out, key_path, dlen);
            out[dlen] = '\0';
            strncat(out, "/.audit_key_history", out_cap - dlen - 1);
            return;
        }
    }
    if (out_cap > 0)
        out[0] = '\0';
}

static hu_error_t append_key_to_history(const char *key_path, int64_t timestamp_s,
                                        const unsigned char *key, size_t key_len) {
    char hist_path[HU_AUDIT_MAX_PATH];
    get_key_history_path(key_path, hist_path, sizeof(hist_path));
    if (hist_path[0] == '\0')
        return HU_ERR_IO;

    char hex_key[128];
    if (key_len * 2 + 1 >= sizeof(hex_key))
        return HU_ERR_INVALID_ARGUMENT;
    hu_hex_encode(key, key_len, hex_key);

    FILE *f = fopen(hist_path, "a");
    if (!f)
        return HU_ERR_IO;
    int n = fprintf(f, "%ld %s\n", (long)timestamp_s, hex_key);
    fclose(f);
    return (n > 0) ? HU_OK : HU_ERR_IO;
}

static hu_error_t load_or_create_audit_key(const char *key_path,
                                           unsigned char key[HU_AUDIT_HMAC_LEN],
                                           hu_allocator_t *alloc) {
    (void)alloc;
    FILE *f = fopen(key_path, "rb");
    if (f) {
        char hex_buf[80];
        size_t n = fread(hex_buf, 1, sizeof(hex_buf) - 1, f);
        fclose(f);
        hex_buf[n] = '\0';
        while (n > 0 && (hex_buf[n - 1] == ' ' || hex_buf[n - 1] == '\t' ||
                         hex_buf[n - 1] == '\n' || hex_buf[n - 1] == '\r'))
            n--;
        hex_buf[n] = '\0';
        size_t dlen;
        hu_error_t err = hu_hex_decode(hex_buf, strlen(hex_buf), key, HU_AUDIT_HMAC_LEN, &dlen);
        if (err != HU_OK || dlen != HU_AUDIT_HMAC_LEN)
            return HU_ERR_CRYPTO_DECRYPT;
        return HU_OK;
    }

    /* Create new key from /dev/urandom */
    f = fopen("/dev/urandom", "rb");
    if (!f)
        return HU_ERR_CRYPTO_ENCRYPT;
    size_t n = fread(key, 1, HU_AUDIT_HMAC_LEN, f);
    fclose(f);
    if (n != HU_AUDIT_HMAC_LEN)
        return HU_ERR_CRYPTO_ENCRYPT;

#ifndef _WIN32
    const char *slash = strrchr(key_path, '/');
    if (slash && slash > key_path) {
        char dir[HU_AUDIT_MAX_PATH];
        size_t dlen = (size_t)(slash - key_path);
        if (dlen < sizeof(dir)) {
            memcpy(dir, key_path, dlen);
            dir[dlen] = '\0';
            (void)mkdir(dir, 0755);
        }
    }
#endif
    f = fopen(key_path, "wb");
    if (!f) {
        audit_secure_zero(key, HU_AUDIT_HMAC_LEN);
        return HU_ERR_IO;
    }
    char hex_out[HU_AUDIT_HMAC_LEN * 2 + 1];
    hu_hex_encode(key, HU_AUDIT_HMAC_LEN, hex_out);
    fwrite(hex_out, 1, HU_AUDIT_HMAC_LEN * 2, f);
    fclose(f);
    return HU_OK;
}

static void bootstrap_prev_hmac_from_log(hu_audit_logger_t *logger) {
    FILE *f = fopen(logger->log_path, "rb");
    if (!f) {
        memset(logger->prev_hmac, 0, HU_AUDIT_HMAC_LEN);
        logger->chain_initialized = true;
        return;
    }
    char line[HU_AUDIT_JSON_BUF_SIZE];
    char last_with_hmac[HU_AUDIT_JSON_BUF_SIZE];
    size_t last_len = 0;
    bool has_hmac = false;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;
        if (strstr(line, "\"hmac\":\"") != NULL) {
            has_hmac = true;
            if (len < sizeof(last_with_hmac)) {
                memcpy(last_with_hmac, line, len + 1);
                last_len = len;
            }
        }
    }
    fclose(f);
    if (has_hmac && last_len >= 10 + 64) {
        const char *p = strstr(last_with_hmac, "\"hmac\":\"");
        if (p) {
            p += 8;
            size_t hex_len;
            hu_error_t err = hu_hex_decode(p, 64, logger->prev_hmac, HU_AUDIT_HMAC_LEN, &hex_len);
            if (err == HU_OK && hex_len == HU_AUDIT_HMAC_LEN) {
                logger->chain_initialized = true;
                return;
            }
        }
    }
    memset(logger->prev_hmac, 0, HU_AUDIT_HMAC_LEN);
    logger->chain_initialized = true;
}

hu_audit_logger_t *hu_audit_logger_create(hu_allocator_t *alloc, const hu_audit_config_t *config,
                                          const char *base_dir) {
    if (!alloc || !config || !base_dir)
        return NULL;

    size_t base_len = strlen(base_dir);
    size_t path_len = strlen(config->log_path);
    if (base_len + path_len + 3 > HU_AUDIT_MAX_PATH)
        return NULL;

    char path[HU_AUDIT_MAX_PATH];
    if (base_len > 0 && base_dir[base_len - 1] == '/')
        snprintf(path, sizeof(path), "%s%s", base_dir, config->log_path);
    else
        snprintf(path, sizeof(path), "%s/%s", base_dir, config->log_path);

    char key_path[HU_AUDIT_MAX_PATH];
    if (base_len + 17 >= sizeof(key_path))
        return NULL;
    build_key_path(key_path, sizeof(key_path), base_dir, base_len);

    hu_audit_logger_t *logger =
        (hu_audit_logger_t *)alloc->alloc(alloc->ctx, sizeof(hu_audit_logger_t));
    if (!logger)
        return NULL;
    memset(logger, 0, sizeof(*logger));

    logger->log_path = (char *)alloc->alloc(alloc->ctx, strlen(path) + 1);
    if (!logger->log_path) {
        alloc->free(alloc->ctx, logger, sizeof(hu_audit_logger_t));
        return NULL;
    }
    memcpy(logger->log_path, path, strlen(path) + 1);

    logger->key_path = (char *)alloc->alloc(alloc->ctx, strlen(key_path) + 1);
    if (!logger->key_path) {
        alloc->free(alloc->ctx, logger->log_path, strlen(path) + 1);
        alloc->free(alloc->ctx, logger, sizeof(hu_audit_logger_t));
        return NULL;
    }
    memcpy(logger->key_path, key_path, strlen(key_path) + 1);

    hu_error_t err = load_or_create_audit_key(key_path, logger->audit_key, alloc);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, logger->key_path, strlen(key_path) + 1);
        alloc->free(alloc->ctx, logger->log_path, strlen(path) + 1);
        alloc->free(alloc->ctx, logger, sizeof(hu_audit_logger_t));
        return NULL;
    }
    bootstrap_prev_hmac_from_log(logger);

    logger->config = *config;
    logger->alloc = alloc;
    return logger;
}

void hu_audit_logger_destroy(hu_audit_logger_t *logger, hu_allocator_t *alloc) {
    if (!logger || !alloc)
        return;
    if (logger->log_path) {
        size_t len = strlen(logger->log_path) + 1;
        alloc->free(alloc->ctx, logger->log_path, len);
    }
    if (logger->key_path) {
        size_t len = strlen(logger->key_path) + 1;
        alloc->free(alloc->ctx, logger->key_path, len);
    }
    audit_secure_zero(logger->audit_key, HU_AUDIT_HMAC_LEN);
    audit_secure_zero(logger->prev_hmac, HU_AUDIT_HMAC_LEN);
    alloc->free(alloc->ctx, logger, sizeof(hu_audit_logger_t));
}

hu_error_t hu_audit_logger_log(hu_audit_logger_t *logger, const hu_audit_event_t *event) {
    if (!logger || !event)
        return HU_ERR_INVALID_ARGUMENT;
    if (!logger->config.enabled)
        return HU_OK;

    /* Check scheduled rotation */
    if (logger->rotation_interval_hours > 0) {
        time_t now = time(NULL);
        if (now - logger->last_rotation_time >= (time_t)(logger->rotation_interval_hours * 3600)) {
            hu_error_t rerr = hu_audit_rotate_key(logger);
            if (rerr != HU_OK)
                return rerr;
        }
    }

    char json_buf[HU_AUDIT_JSON_BUF_SIZE];
    size_t json_len = hu_audit_event_write_json(event, json_buf, sizeof(json_buf));
    if (json_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* HMAC chain: HMAC(prev_hmac || entry_json without hmac) */
    unsigned char hmac_input[32 + HU_AUDIT_JSON_BUF_SIZE];
    if (sizeof(hmac_input) < HU_AUDIT_HMAC_LEN + json_len)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(hmac_input, logger->prev_hmac, HU_AUDIT_HMAC_LEN);
    memcpy(hmac_input + HU_AUDIT_HMAC_LEN, json_buf, json_len);
    hu_hmac_sha256(logger->audit_key, HU_AUDIT_HMAC_LEN, hmac_input, HU_AUDIT_HMAC_LEN + json_len,
                   logger->prev_hmac);

    /* Replace trailing }} with },"hmac":"<64hex>"} */
    char hmac_hex[65];
    hu_hex_encode(logger->prev_hmac, HU_AUDIT_HMAC_LEN, hmac_hex);
    if (json_len < 2)
        return HU_ERR_INVALID_ARGUMENT;
    size_t base = json_len - 1; /* position of final } */
    size_t needed = 9 + 64 + 2; /* ,"hmac":" + hex + "} */
    if (base + needed >= sizeof(json_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(json_buf + base, ",\"hmac\":\"", 9);
    memcpy(json_buf + base + 9, hmac_hex, 64);
    json_buf[base + 9 + 64] = '"';
    json_buf[base + 9 + 64 + 1] = '}';
    json_buf[base + 9 + 64 + 2] = '\0';
    json_len = base + 9 + 64 + 2;

    FILE *f = fopen(logger->log_path, "a");
    if (!f)
        return HU_ERR_IO;

    size_t written = fwrite(json_buf, 1, json_len, f);
    if (written != json_len) {
        fclose(f);
        return HU_ERR_IO;
    }
    if (fputc('\n', f) == EOF) {
        fclose(f);
        return HU_ERR_IO;
    }
    fflush(f);
    fclose(f);
    return HU_OK;
}

hu_error_t hu_audit_logger_log_command(hu_audit_logger_t *logger, const hu_audit_cmd_log_t *entry) {
    if (!logger || !entry)
        return HU_ERR_INVALID_ARGUMENT;

    hu_audit_event_t ev;
    hu_audit_event_init(&ev, HU_AUDIT_COMMAND_EXECUTION);
    hu_audit_event_with_actor(&ev, entry->channel, NULL, NULL);
    hu_audit_event_with_action(&ev, entry->command, entry->risk_level, entry->approved,
                               entry->allowed);
    hu_audit_event_with_result(&ev, entry->success, -1, entry->duration_ms, NULL);

    return hu_audit_logger_log(logger, &ev);
}

void hu_audit_set_rotation_interval(hu_audit_logger_t *logger, uint32_t hours) {
    if (!logger)
        return;
    logger->rotation_interval_hours = hours;
    if (hours > 0)
        logger->last_rotation_time = time(NULL);
}

#if defined(HU_IS_TEST) && HU_IS_TEST
void hu_audit_test_set_last_rotation_epoch(hu_audit_logger_t *logger, time_t epoch) {
    if (logger)
        logger->last_rotation_time = epoch;
}
#endif

hu_error_t hu_audit_rotate_key(hu_audit_logger_t *logger) {
    if (!logger)
        return HU_ERR_INVALID_ARGUMENT;

    /* 1. Generate new key from /dev/urandom */
    unsigned char new_key[HU_AUDIT_HMAC_LEN];
    FILE *frand = fopen("/dev/urandom", "rb");
    if (!frand)
        return HU_ERR_CRYPTO_ENCRYPT;
    size_t n = fread(new_key, 1, HU_AUDIT_HMAC_LEN, frand);
    fclose(frand);
    if (n != HU_AUDIT_HMAC_LEN) {
        audit_secure_zero(new_key, HU_AUDIT_HMAC_LEN);
        return HU_ERR_CRYPTO_ENCRYPT;
    }

    /* 2. Append OLD key to history before overwriting */
    hu_error_t err = append_key_to_history(logger->key_path, (int64_t)time(NULL), logger->audit_key,
                                           HU_AUDIT_HMAC_LEN);
    if (err != HU_OK) {
        audit_secure_zero(new_key, HU_AUDIT_HMAC_LEN);
        return err;
    }

    /* 3. Compute old_key_hash = SHA256(old key) */
    unsigned char old_key_hash[32];
    hu_sha256(logger->audit_key, HU_AUDIT_HMAC_LEN, old_key_hash);
    char old_hash_hex[65];
    hu_hex_encode(old_key_hash, 32, old_hash_hex);
    old_hash_hex[64] = '\0';

    /* 4. Build key_rotation entry (without hmac) */
    char rotation_json[512];
    time_t now = time(NULL);
    int rn = snprintf(rotation_json, sizeof(rotation_json),
                      "{\"type\":\"key_rotation\",\"timestamp_s\":%ld,\"old_key_hash\":\"%s\"}",
                      (long)now, old_hash_hex);
    if (rn < 0 || (size_t)rn >= sizeof(rotation_json)) {
        audit_secure_zero(new_key, HU_AUDIT_HMAC_LEN);
        return HU_ERR_INVALID_ARGUMENT;
    }
    size_t rotation_len = (size_t)rn;

    /* 5. Compute HMAC with OLD key: HMAC(prev_hmac || rotation_json) */
    unsigned char hmac_input[32 + 512];
    if (sizeof(hmac_input) < HU_AUDIT_HMAC_LEN + rotation_len) {
        audit_secure_zero(new_key, HU_AUDIT_HMAC_LEN);
        return HU_ERR_INVALID_ARGUMENT;
    }
    memcpy(hmac_input, logger->prev_hmac, HU_AUDIT_HMAC_LEN);
    memcpy(hmac_input + HU_AUDIT_HMAC_LEN, rotation_json, rotation_len);
    unsigned char rotation_hmac[HU_AUDIT_HMAC_LEN];
    hu_hmac_sha256(logger->audit_key, HU_AUDIT_HMAC_LEN, hmac_input,
                   HU_AUDIT_HMAC_LEN + rotation_len, rotation_hmac);

    /* 6. Append HMAC to rotation entry */
    char hmac_hex[65];
    hu_hex_encode(rotation_hmac, HU_AUDIT_HMAC_LEN, hmac_hex);
    char full_entry[600];
    rn = snprintf(
        full_entry, sizeof(full_entry),
        "{\"type\":\"key_rotation\",\"timestamp_s\":%ld,\"old_key_hash\":\"%s\",\"hmac\":\"%s\"}\n",
        (long)now, old_hash_hex, hmac_hex);
    if (rn < 0 || (size_t)rn >= sizeof(full_entry)) {
        audit_secure_zero(new_key, HU_AUDIT_HMAC_LEN);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* 7. Write rotation entry to log */
    FILE *flog = fopen(logger->log_path, "a");
    if (!flog) {
        audit_secure_zero(new_key, HU_AUDIT_HMAC_LEN);
        return HU_ERR_IO;
    }
    size_t written = fwrite(full_entry, 1, (size_t)rn, flog);
    fflush(flog);
    fclose(flog);
    if (written != (size_t)rn) {
        audit_secure_zero(new_key, HU_AUDIT_HMAC_LEN);
        return HU_ERR_IO;
    }

    /* 8. Update prev_hmac for next entry (rotation entry's hmac becomes new chain head) */
    memcpy(logger->prev_hmac, rotation_hmac, HU_AUDIT_HMAC_LEN);

    /* 9. Save new key to key file */
    FILE *fkey = fopen(logger->key_path, "wb");
    if (!fkey) {
        audit_secure_zero(new_key, HU_AUDIT_HMAC_LEN);
        return HU_ERR_IO;
    }
    char hex_out[HU_AUDIT_HMAC_LEN * 2 + 1];
    hu_hex_encode(new_key, HU_AUDIT_HMAC_LEN, hex_out);
    fwrite(hex_out, 1, HU_AUDIT_HMAC_LEN * 2, fkey);
    fclose(fkey);

    /* 10. Clear old key and update logger */
    audit_secure_zero(logger->audit_key, HU_AUDIT_HMAC_LEN);
    memcpy(logger->audit_key, new_key, HU_AUDIT_HMAC_LEN);
    audit_secure_zero(new_key, HU_AUDIT_HMAC_LEN);
    logger->last_rotation_time = now;
    return HU_OK;
}

hu_error_t hu_audit_load_key(const char *base_dir, unsigned char key[32]) {
    if (!base_dir || !key)
        return HU_ERR_INVALID_ARGUMENT;
    char key_path[HU_AUDIT_MAX_PATH];
    size_t base_len = strlen(base_dir);
    if (base_len + 17 >= sizeof(key_path))
        return HU_ERR_INVALID_ARGUMENT;
    build_key_path(key_path, sizeof(key_path), base_dir, base_len);
    FILE *f = fopen(key_path, "rb");
    if (!f)
        return HU_ERR_IO;
    char hex_buf[80];
    size_t n = fread(hex_buf, 1, sizeof(hex_buf) - 1, f);
    fclose(f);
    hex_buf[n] = '\0';
    while (n > 0 && (hex_buf[n - 1] == ' ' || hex_buf[n - 1] == '\t' || hex_buf[n - 1] == '\n' ||
                     hex_buf[n - 1] == '\r'))
        n--;
    hex_buf[n] = '\0';
    size_t dlen;
    hu_error_t err = hu_hex_decode(hex_buf, strlen(hex_buf), key, 32, &dlen);
    if (err != HU_OK || dlen != 32)
        return HU_ERR_CRYPTO_DECRYPT;
    return HU_OK;
}

static void build_key_path(char *out, size_t cap, const char *base, size_t blen) {
    const char suffix[] = ".audit_hmac_key";
    const size_t slen = sizeof(suffix) - 1;
    memcpy(out, base, blen);
    if (blen > 0 && base[blen - 1] != '/') {
        out[blen] = '/';
        memcpy(out + blen + 1, suffix, slen + 1);
    } else {
        memcpy(out + blen, suffix, slen + 1);
    }
    (void)cap;
}

static hu_error_t
load_keys_from_base_dir(const char *base_dir,
                        unsigned char keys[HU_AUDIT_MAX_KEY_HISTORY][HU_AUDIT_HMAC_LEN],
                        size_t *out_count) {
    char key_path[HU_AUDIT_MAX_PATH];
    size_t base_len = strlen(base_dir);
    if (base_len + 17 >= sizeof(key_path))
        return HU_ERR_INVALID_ARGUMENT;
    build_key_path(key_path, sizeof(key_path), base_dir, base_len);

    char hist_path[HU_AUDIT_MAX_PATH];
    get_key_history_path(key_path, hist_path, sizeof(hist_path));

    size_t idx = 0;

    /* Load keys from history first (oldest first) */
    FILE *fh = fopen(hist_path, "rb");
    if (fh) {
        char hist_line[HU_AUDIT_KEY_HISTORY_LINE];
        while (fgets(hist_line, sizeof(hist_line), fh) && idx < HU_AUDIT_MAX_KEY_HISTORY) {
            size_t ll = strlen(hist_line);
            while (ll > 0 && (hist_line[ll - 1] == '\n' || hist_line[ll - 1] == '\r'))
                hist_line[--ll] = '\0';
            if (ll < 70)
                continue; /* Need timestamp + space + 64 hex */
            const char *space = strchr(hist_line, ' ');
            if (!space || (size_t)(space - hist_line) < 10)
                continue;
            const char *hex = space + 1;
            size_t dlen;
            hu_error_t err = hu_hex_decode(hex, strlen(hex), keys[idx], HU_AUDIT_HMAC_LEN, &dlen);
            if (err == HU_OK && dlen == HU_AUDIT_HMAC_LEN)
                idx++;
        }
        fclose(fh);
    }

    /* Add current key from .audit_hmac_key */
    FILE *fk = fopen(key_path, "rb");
    if (!fk)
        return HU_ERR_IO;
    char hex_buf[80];
    size_t n = fread(hex_buf, 1, sizeof(hex_buf) - 1, fk);
    fclose(fk);
    hex_buf[n] = '\0';
    while (n > 0 && (hex_buf[n - 1] == ' ' || hex_buf[n - 1] == '\t' || hex_buf[n - 1] == '\n' ||
                     hex_buf[n - 1] == '\r'))
        hex_buf[--n] = '\0';
    size_t dlen;
    hu_error_t err = hu_hex_decode(hex_buf, strlen(hex_buf), keys[idx], HU_AUDIT_HMAC_LEN, &dlen);
    if (err != HU_OK || dlen != HU_AUDIT_HMAC_LEN)
        return HU_ERR_CRYPTO_DECRYPT;
    idx++;
    *out_count = idx;
    return HU_OK;
}

static void derive_base_dir_from_log_path(const char *audit_file_path, char *base_dir, size_t cap) {
    const char *slash = strrchr(audit_file_path, '/');
    if (slash && slash > audit_file_path) {
        size_t dlen = (size_t)(slash - audit_file_path);
        if (dlen + 1 <= cap) {
            memcpy(base_dir, audit_file_path, dlen);
            base_dir[dlen] = '\0';
            return;
        }
    }
    if (cap > 0)
        base_dir[0] = '\0';
}

hu_error_t hu_audit_verify_chain(const char *audit_file_path, const unsigned char *key) {
    if (!audit_file_path)
        return HU_ERR_INVALID_ARGUMENT;

    /* Single-key mode when key provided */
    unsigned char single_key[HU_AUDIT_HMAC_LEN];
    const unsigned char *cur_key;
    size_t key_count = 1;
    size_t key_index = 0;
    unsigned char key_ring[HU_AUDIT_MAX_KEY_HISTORY][HU_AUDIT_HMAC_LEN];

    if (key) {
        memcpy(single_key, key, HU_AUDIT_HMAC_LEN);
        cur_key = single_key;
    } else {
        char base_dir[HU_AUDIT_MAX_PATH];
        derive_base_dir_from_log_path(audit_file_path, base_dir, sizeof(base_dir));
        if (base_dir[0] == '\0')
            return HU_ERR_INVALID_ARGUMENT;
        hu_error_t lerr = load_keys_from_base_dir(base_dir, key_ring, &key_count);
        if (lerr != HU_OK)
            return lerr;
        cur_key = key_ring[0];
    }

    FILE *f = fopen(audit_file_path, "rb");
    if (!f)
        return HU_ERR_IO;

    unsigned char prev_hmac[HU_AUDIT_HMAC_LEN];
    memset(prev_hmac, 0, HU_AUDIT_HMAC_LEN);

    char line[HU_AUDIT_JSON_BUF_SIZE];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;

        const char *hmac_start = strstr(line, "\"hmac\":\"");
        if (!hmac_start) {
            fclose(f);
            return HU_ERR_PARSE; /* Entry without hmac - chain broken */
        }
        const char *hmac_hex = hmac_start + 8;
        if (len < (size_t)(hmac_hex - line) + 64)
            continue; /* Malformed, skip (would fail decode) */

        /* Build entry without hmac: everything before ,"hmac":" plus } */
        if (hmac_start <= line + 1) {
            fclose(f);
            return HU_ERR_PARSE;
        }
        size_t prefix_len = (size_t)(hmac_start - line - 1);
        if (prefix_len == 0) {
            fclose(f);
            return HU_ERR_PARSE;
        }

        char entry_without_hmac[HU_AUDIT_JSON_BUF_SIZE];
        if (prefix_len >= sizeof(entry_without_hmac) - 2) {
            fclose(f);
            return HU_ERR_PARSE;
        }
        memcpy(entry_without_hmac, line, prefix_len);
        entry_without_hmac[prefix_len] = '}';
        entry_without_hmac[prefix_len + 1] = '\0';
        size_t entry_len = prefix_len + 1;

        /* Compute HMAC(prev_hmac || entry_without_hmac) */
        unsigned char hmac_input[32 + HU_AUDIT_JSON_BUF_SIZE];
        if (sizeof(hmac_input) < HU_AUDIT_HMAC_LEN + entry_len) {
            fclose(f);
            return HU_ERR_INVALID_ARGUMENT;
        }
        memcpy(hmac_input, prev_hmac, HU_AUDIT_HMAC_LEN);
        memcpy(hmac_input + HU_AUDIT_HMAC_LEN, entry_without_hmac, entry_len);
        unsigned char computed[HU_AUDIT_HMAC_LEN];
        hu_hmac_sha256(cur_key, HU_AUDIT_HMAC_LEN, hmac_input, HU_AUDIT_HMAC_LEN + entry_len,
                       computed);

        /* Decode stored hmac and compare */
        unsigned char stored[HU_AUDIT_HMAC_LEN];
        size_t stored_len;
        hu_error_t err = hu_hex_decode(hmac_hex, 64, stored, HU_AUDIT_HMAC_LEN, &stored_len);
        if (err != HU_OK || stored_len != HU_AUDIT_HMAC_LEN) {
            fclose(f);
            return HU_ERR_CRYPTO_DECRYPT;
        }
        unsigned char diff = 0;
        for (int i = 0; i < HU_AUDIT_HMAC_LEN; i++)
            diff |= computed[i] ^ stored[i];
        if (diff != 0) {
            fclose(f);
            return HU_ERR_CRYPTO_DECRYPT; /* Tampering detected */
        }
        memcpy(prev_hmac, computed, HU_AUDIT_HMAC_LEN);

        /* If key_rotation entry and multi-key mode, advance to next key */
        if (!key && key_index + 1 < key_count &&
            strstr(line, "\"type\":\"key_rotation\"") != NULL) {
            key_index++;
            cur_key = key_ring[key_index];
        }
    }
    fclose(f);
    return HU_OK;
}
