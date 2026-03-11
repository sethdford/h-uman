#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/persona/training.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HU_TRAINING_ESCAPE_BUF 2048
#define HU_TRAINING_SQL_BUF 4096
#define HU_STYLE_DRIFT_DEFAULT_THRESHOLD 0.3
#define HU_STYLE_DRIFT_DIM_COUNT 5.0

/* --- F163: Style Drift --- */

static double clamp01(double x) {
    if (x < 0.0)
        return 0.0;
    if (x > 1.0)
        return 1.0;
    return x;
}

double hu_style_drift_score(const hu_style_snapshot_t *baseline,
                            const hu_style_snapshot_t *current) {
    if (!baseline || !current)
        return 0.0;
    double sum = 0.0;
    sum += fabs(baseline->avg_msg_length - current->avg_msg_length);
    sum += fabs(baseline->emoji_rate - current->emoji_rate);
    sum += fabs(baseline->abbreviation_rate - current->abbreviation_rate);
    sum += fabs(baseline->question_rate - current->question_rate);
    sum += fabs(baseline->capitalization_rate - current->capitalization_rate);
    return clamp01(sum / HU_STYLE_DRIFT_DIM_COUNT);
}

bool hu_style_drift_significant(double drift_score, double threshold) {
    return drift_score > threshold;
}

static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap,
                              size_t *out_len) {
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < cap; i++) {
        if (s[i] == '\'') {
            buf[pos++] = '\'';
            buf[pos++] = '\'';
        } else {
            buf[pos++] = s[i];
        }
    }
    buf[pos] = '\0';
    *out_len = pos;
}

hu_error_t hu_style_drift_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS style_snapshots (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    avg_msg_length REAL NOT NULL,\n"
        "    emoji_rate REAL NOT NULL,\n"
        "    abbreviation_rate REAL NOT NULL,\n"
        "    question_rate REAL NOT NULL,\n"
        "    capitalization_rate REAL NOT NULL,\n"
        "    timestamp INTEGER NOT NULL\n"
        ");\n"
        "CREATE INDEX IF NOT EXISTS idx_style_snapshots_timestamp ON style_snapshots(timestamp);\n";

    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_style_drift_insert_sql(const hu_style_snapshot_t *snap, char *buf, size_t cap,
                                     size_t *out_len) {
    if (!snap || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                     "INSERT INTO style_snapshots (avg_msg_length, emoji_rate, "
                     "abbreviation_rate, question_rate, capitalization_rate, timestamp) "
                     "VALUES (%f, %f, %f, %f, %f, %llu)",
                     snap->avg_msg_length, snap->emoji_rate, snap->abbreviation_rate,
                     snap->question_rate, snap->capitalization_rate,
                     (unsigned long long)snap->timestamp);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

/* --- F165-F166: Behavioral Cloning Calibration --- */

hu_error_t hu_clone_calibrate(hu_allocator_t *alloc, const char **param_names,
                              const size_t *param_lens, const double *targets,
                              const double *currents, size_t param_count,
                              hu_clone_calibration_t *out) {
    if (!alloc || !targets || !currents || !out || param_count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < param_count; i++) {
        double target = targets[i];
        double current = currents[i];
        double delta = target - current;

        out[i].target_value = target;
        out[i].current_value = current;
        out[i].delta = delta;

        if (param_names && param_names[i]) {
            size_t plen = param_lens ? param_lens[i] : strlen(param_names[i]);
            out[i].parameter = hu_strndup(alloc, param_names[i], plen);
            out[i].parameter_len = plen;
        } else {
            static const char *defaults[] = {"formality", "emoji_freq", "msg_length"};
            const char *name = i < 3 ? defaults[i] : "param";
            size_t nlen = strlen(name);
            out[i].parameter = hu_strndup(alloc, name, nlen);
            out[i].parameter_len = nlen;
        }
    }
    return HU_OK;
}

hu_error_t hu_clone_build_adjustment_prompt(hu_allocator_t *alloc,
                                            const hu_clone_calibration_t *cals, size_t count,
                                            char **out, size_t *out_len) {
    if (!alloc || !cals || !out || !out_len || count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[4096];
    size_t pos = 0;
    int n = snprintf(buf + pos, sizeof(buf) - pos, "[CALIBRATION NEEDED]: ");
    if (n < 0 || (size_t)n >= sizeof(buf) - pos)
        return HU_ERR_INVALID_ARGUMENT;
    pos += (size_t)n;

    for (size_t i = 0; i < count && pos + 2 < sizeof(buf); i++) {
        const char *param = cals[i].parameter ? cals[i].parameter : "param";
        double delta = cals[i].delta;

        if (delta > 0.0) {
            n = snprintf(buf + pos, sizeof(buf) - pos, "%s: too low by %.2f — increase %s. ",
                        param, delta, param);
        } else if (delta < 0.0) {
            n = snprintf(buf + pos, sizeof(buf) - pos, "%s: too high by %.2f — reduce %s. ",
                        param, -delta, param);
        } else {
            continue;
        }
        if (n < 0 || (size_t)n >= sizeof(buf) - pos)
            break;
        pos += (size_t)n;
    }

    buf[pos] = '\0';
    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}

void hu_clone_calibration_deinit(hu_allocator_t *alloc, hu_clone_calibration_t *cal) {
    if (!alloc || !cal)
        return;
    if (cal->parameter) {
        hu_str_free(alloc, cal->parameter);
        cal->parameter = NULL;
        cal->parameter_len = 0;
    }
}

/* --- F144-F146: Persona LoRA --- */

#define HU_LORA_DEFAULT_MIN_MESSAGES 500
#define HU_LORA_DEFAULT_EPOCHS 3
#define HU_LORA_DEFAULT_LEARNING_RATE 2e-4
#define HU_LORA_DEFAULT_RANK 16

hu_lora_config_t hu_lora_default_config(void) {
    hu_lora_config_t c;
    memset(&c, 0, sizeof(c));
    c.min_training_messages = HU_LORA_DEFAULT_MIN_MESSAGES;
    c.epochs = HU_LORA_DEFAULT_EPOCHS;
    c.learning_rate = HU_LORA_DEFAULT_LEARNING_RATE;
    c.rank = HU_LORA_DEFAULT_RANK;
    return c;
}

hu_lora_status_t hu_lora_check_readiness(uint32_t messages_collected,
                                        uint32_t min_required) {
    if (messages_collected == 0)
        return HU_LORA_NOT_STARTED;
    if (messages_collected < min_required)
        return HU_LORA_COLLECTING;
    return HU_LORA_READY;
}

hu_error_t hu_lora_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS lora_training_samples (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    message TEXT NOT NULL,\n"
        "    context TEXT,\n"
        "    timestamp INTEGER NOT NULL\n"
        ");\n"
        "CREATE INDEX IF NOT EXISTS idx_lora_samples_timestamp ON lora_training_samples(timestamp);\n";

    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_lora_insert_training_sample_sql(const char *message, size_t msg_len,
                                             const char *context, size_t ctx_len,
                                             uint64_t timestamp, char *buf, size_t cap,
                                             size_t *out_len) {
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    if (!message)
        return HU_ERR_INVALID_ARGUMENT;

    char msg_esc[HU_TRAINING_ESCAPE_BUF];
    char ctx_esc[HU_TRAINING_ESCAPE_BUF];
    size_t me_len, ce_len;

    escape_sql_string(message, msg_len, msg_esc, sizeof(msg_esc), &me_len);
    escape_sql_string(context ? context : "", ctx_len, ctx_esc, sizeof(ctx_esc), &ce_len);

    int n = snprintf(buf, cap,
                     "INSERT INTO lora_training_samples (message, context, timestamp) "
                     "VALUES ('%s', '%s', %llu)",
                     msg_esc, ctx_esc, (unsigned long long)timestamp);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_lora_query_status_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] =
        "SELECT COUNT(*) FROM lora_training_samples";

    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_lora_build_prompt(hu_allocator_t *alloc, const hu_lora_state_t *state,
                                char **out, size_t *out_len) {
    if (!alloc || !state || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t cap = 512;
    char *buf = alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int n;
    switch (state->status) {
    case HU_LORA_COLLECTING:
        n = snprintf(buf, cap,
                     "[LORA STATUS: Collecting training data. %u/%u messages. "
                     "Continue producing natural messages.]",
                     state->messages_collected, state->messages_needed);
        break;
    case HU_LORA_TRAINING:
        n = snprintf(buf, cap, "[LORA STATUS: Training in progress.]");
        break;
    case HU_LORA_READY:
        n = snprintf(buf, cap, "[LORA STATUS: Ready for training.]");
        break;
    case HU_LORA_ACTIVE:
        n = snprintf(buf, cap, "[LORA STATUS: Active adapter loaded.]");
        break;
    default:
        n = snprintf(buf, cap, "[LORA STATUS: Not started.]");
        break;
    }

    if (n < 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out = buf;
    *out_len = (size_t)n;
    return HU_OK;
}

const char *hu_lora_status_str(hu_lora_status_t s) {
    switch (s) {
    case HU_LORA_NOT_STARTED:
        return "not_started";
    case HU_LORA_COLLECTING:
        return "collecting";
    case HU_LORA_TRAINING:
        return "training";
    case HU_LORA_READY:
        return "ready";
    case HU_LORA_ACTIVE:
        return "active";
    default:
        return "unknown";
    }
}

void hu_lora_state_deinit(hu_allocator_t *alloc, hu_lora_state_t *s) {
    if (!alloc || !s)
        return;
    if (s->model_path) {
        hu_str_free(alloc, s->model_path);
        s->model_path = NULL;
        s->model_path_len = 0;
    }
}

void hu_lora_config_deinit(hu_allocator_t *alloc, hu_lora_config_t *c) {
    if (!alloc || !c)
        return;
    if (c->base_model) {
        hu_str_free(alloc, c->base_model);
        c->base_model = NULL;
        c->base_model_len = 0;
    }
}
