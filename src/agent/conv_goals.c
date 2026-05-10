#include "human/agent/conv_goals.h"
#include "human/core/string.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HU_CONV_GOALS_ESCAPE_BUF 2048

/* Escape single quotes by doubling for SQL. Returns length or 0 on overflow. */
static size_t escape_sql_string(const char *s, size_t len, char *out, size_t out_cap) {
    size_t n = 0;
    (void)hu_sql_quote_escape_into(s, len, out, out_cap, &n);
    return n;
}

hu_error_t hu_conv_goals_create_table_sql(char *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS conversation_goals (\n"
        "    id INTEGER PRIMARY KEY,\n"
        "    contact_id TEXT NOT NULL,\n"
        "    description TEXT NOT NULL,\n"
        "    success_signal TEXT,\n"
        "    status TEXT DEFAULT 'pending',\n"
        "    priority TEXT DEFAULT 'medium',\n"
        "    created_at INTEGER NOT NULL,\n"
        "    target_by INTEGER,\n"
        "    achieved_at INTEGER,\n"
        "    attempts INTEGER DEFAULT 0,\n"
        "    max_attempts INTEGER DEFAULT 5\n"
        ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_conv_goals_insert_sql(const hu_conv_goal_t *goal, char *buf, size_t cap,
                                   size_t *out_len)
{
    if (!goal || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    if (!goal->contact_id || !goal->description)
        return HU_ERR_INVALID_ARGUMENT;

    char esc_contact[HU_CONV_GOALS_ESCAPE_BUF];
    char esc_desc[HU_CONV_GOALS_ESCAPE_BUF];
    char esc_signal[HU_CONV_GOALS_ESCAPE_BUF];

    size_t contact_len = goal->contact_id_len ? goal->contact_id_len : strlen(goal->contact_id);
    if (escape_sql_string(goal->contact_id, contact_len, esc_contact, sizeof(esc_contact)) == 0 &&
        contact_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    size_t desc_len = goal->description_len ? goal->description_len : strlen(goal->description);
    if (escape_sql_string(goal->description, desc_len, esc_desc, sizeof(esc_desc)) == 0 &&
        desc_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *signal = goal->success_signal ? goal->success_signal : "";
    size_t signal_len = goal->success_signal_len ? goal->success_signal_len : strlen(signal);
    escape_sql_string(signal, signal_len, esc_signal, sizeof(esc_signal));

    const char *status_str = hu_goal_status_str(goal->status);
    const char *priority_str = hu_goal_priority_str(goal->priority);

    uint8_t max_attempts = goal->max_attempts > 0 ? goal->max_attempts : 5;

    int n = snprintf(buf, cap,
                     "INSERT INTO conversation_goals (id, contact_id, description, success_signal, "
                     "status, priority, created_at, target_by, achieved_at, attempts, max_attempts) "
                     "VALUES (%lld, '%s', '%s', '%s', '%s', '%s', %llu, %llu, %llu, %u, %u)",
                     (long long)goal->id, esc_contact, esc_desc, esc_signal, status_str,
                     priority_str, (unsigned long long)goal->created_at,
                     (unsigned long long)goal->target_by, (unsigned long long)goal->achieved_at,
                     (unsigned)goal->attempts, (unsigned)max_attempts);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_conv_goals_update_status_sql(int64_t goal_id, hu_goal_status_t new_status,
                                           char *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;
    const char *status_str = hu_goal_status_str(new_status);
    int n = snprintf(buf, cap, "UPDATE conversation_goals SET status = '%s' WHERE id = %lld",
                     status_str, (long long)goal_id);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_conv_goals_query_active_sql(const char *contact_id, size_t contact_id_len,
                                          char *buf, size_t cap, size_t *out_len)
{
    if (!contact_id || contact_id_len == 0 || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char sanitized[257];
    size_t out_pos = 0;
    for (size_t i = 0; i < contact_id_len && out_pos < 256; i++) {
        if (contact_id[i] == '\'') {
            if (out_pos + 2 > 256)
                break;
            sanitized[out_pos++] = '\'';
            sanitized[out_pos++] = '\'';
        } else {
            if (out_pos + 1 > 256)
                break;
            sanitized[out_pos++] = contact_id[i];
        }
    }
    sanitized[out_pos] = '\0';

    int n = snprintf(buf, cap,
                     "SELECT id, contact_id, description, success_signal, status, priority, "
                     "created_at, target_by, achieved_at, attempts, max_attempts "
                     "FROM conversation_goals WHERE contact_id = '%s' "
                     "AND status IN ('pending', 'in_progress') "
                     "ORDER BY priority DESC, created_at ASC",
                     sanitized);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

bool hu_conv_goal_should_abandon(const hu_conv_goal_t *goal, uint64_t now_ms)
{
    if (!goal)
        return false;
    if (goal->attempts >= goal->max_attempts)
        return true;
    if (goal->target_by > 0 && now_ms > goal->target_by)
        return true;
    return false;
}

hu_error_t hu_conv_goal_record_attempt(hu_conv_goal_t *goal)
{
    if (!goal)
        return HU_ERR_INVALID_ARGUMENT;
    goal->attempts++;
    if (goal->attempts > goal->max_attempts)
        goal->status = HU_GOAL_ABANDONED;
    return HU_OK;
}

double hu_conv_goal_urgency(const hu_conv_goal_t *goal, uint64_t now_ms)
{
    if (!goal)
        return 0.0;

    double priority_score;
    switch (goal->priority) {
    case HU_GOAL_LOW:
        priority_score = 0.2;
        break;
    case HU_GOAL_MEDIUM:
        priority_score = 0.5;
        break;
    case HU_GOAL_HIGH:
        priority_score = 0.8;
        break;
    case HU_GOAL_CRITICAL:
        priority_score = 1.0;
        break;
    default:
        priority_score = 0.5;
    }

    double deadline_score;
    if (goal->target_by > 0) {
        int64_t days_left = (int64_t)((goal->target_by - now_ms) / 86400000ULL);
        if (days_left <= 0)
            deadline_score = 1.0;
        else if (days_left <= 1)
            deadline_score = 0.9;
        else if (days_left <= 3)
            deadline_score = 0.7;
        else if (days_left <= 7)
            deadline_score = 0.4;
        else
            deadline_score = 0.2;
    } else {
        deadline_score = 0.3;
    }

    uint8_t max = goal->max_attempts > 0 ? goal->max_attempts : 5;
    double attempt_ratio = (double)goal->attempts / (double)max;
    double attempt_score = 1.0 - attempt_ratio;

    double urgency = 0.4 * priority_score + 0.3 * deadline_score + 0.3 * attempt_score;
    if (urgency < 0.0)
        return 0.0;
    if (urgency > 1.0)
        return 1.0;
    return urgency;
}

hu_error_t hu_conv_goals_build_prompt(hu_allocator_t *alloc, const hu_conv_goal_t *goals,
                                      size_t goal_count, char **out, size_t *out_len)
{
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (!goals || goal_count == 0) {
        *out = hu_strndup(alloc, "[No active conversation goals with this contact]", 48);
        if (!*out)
            return HU_ERR_OUT_OF_MEMORY;
        *out_len = 48;
        return HU_OK;
    }

    char buf[4096];
    size_t pos = 0;
    const char *hdr = "[CONVERSATION GOALS with this contact]:\n";
    size_t hdr_len = strlen(hdr);
    if (pos + hdr_len >= sizeof(buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf + pos, hdr, hdr_len + 1);
    pos += hdr_len;

    for (size_t i = 0; i < goal_count && pos < sizeof(buf) - 1; i++) {
        const char *desc = goals[i].description ? goals[i].description : "(no description)";
        size_t desc_len = goals[i].description_len ? goals[i].description_len : strlen(desc);
        const char *priority_display;
        switch (goals[i].priority) {
        case HU_GOAL_LOW:
            priority_display = "LOW";
            break;
        case HU_GOAL_MEDIUM:
            priority_display = "MEDIUM";
            break;
        case HU_GOAL_HIGH:
            priority_display = "HIGH";
            break;
        case HU_GOAL_CRITICAL:
            priority_display = "CRITICAL";
            break;
        default:
            priority_display = "MEDIUM";
        }
        uint8_t max = goals[i].max_attempts > 0 ? goals[i].max_attempts : 5;

        int n = snprintf(buf + pos, sizeof(buf) - pos, "%zu. [%s] %.*s (%u/%u attempts)\n",
                        i + 1, priority_display, (int)desc_len, desc, (unsigned)goals[i].attempts,
                        (unsigned)max);
        if (n < 0 || (size_t)n >= sizeof(buf) - pos)
            break;
        pos += (size_t)n;

        if (goals[i].success_signal && goals[i].success_signal_len > 0) {
            int n2 = snprintf(buf + pos, sizeof(buf) - pos, "   Success signal: %.*s\n",
                             (int)goals[i].success_signal_len, goals[i].success_signal);
            if (n2 >= 0 && (size_t)n2 < sizeof(buf) - pos)
                pos += (size_t)n2;
        }
    }

    const char *footer = "\nSteer conversation toward these goals naturally. Don't force topics.\n";
    size_t footer_len = strlen(footer);
    if (pos + footer_len < sizeof(buf)) {
        memcpy(buf + pos, footer, footer_len + 1);
        pos += footer_len;
    }

    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}

const char *hu_goal_status_str(hu_goal_status_t status)
{
    switch (status) {
    case HU_GOAL_PENDING:
        return "pending";
    case HU_GOAL_IN_PROGRESS:
        return "in_progress";
    case HU_GOAL_ACHIEVED:
        return "achieved";
    case HU_GOAL_ABANDONED:
        return "abandoned";
    case HU_GOAL_DEFERRED:
        return "deferred";
    default:
        return "pending";
    }
}

bool hu_goal_status_from_str(const char *str, hu_goal_status_t *out)
{
    if (!str || !out)
        return false;
    if (strcmp(str, "pending") == 0) {
        *out = HU_GOAL_PENDING;
        return true;
    }
    if (strcmp(str, "in_progress") == 0) {
        *out = HU_GOAL_IN_PROGRESS;
        return true;
    }
    if (strcmp(str, "achieved") == 0) {
        *out = HU_GOAL_ACHIEVED;
        return true;
    }
    if (strcmp(str, "abandoned") == 0) {
        *out = HU_GOAL_ABANDONED;
        return true;
    }
    if (strcmp(str, "deferred") == 0) {
        *out = HU_GOAL_DEFERRED;
        return true;
    }
    return false;
}

const char *hu_goal_priority_str(hu_goal_priority_t priority)
{
    switch (priority) {
    case HU_GOAL_LOW:
        return "low";
    case HU_GOAL_MEDIUM:
        return "medium";
    case HU_GOAL_HIGH:
        return "high";
    case HU_GOAL_CRITICAL:
        return "critical";
    default:
        return "medium";
    }
}

void hu_conv_goal_deinit(hu_allocator_t *alloc, hu_conv_goal_t *goal)
{
    if (!alloc || !goal)
        return;
    if (goal->contact_id)
        hu_str_free(alloc, goal->contact_id);
    if (goal->description)
        hu_str_free(alloc, goal->description);
    if (goal->success_signal)
        hu_str_free(alloc, goal->success_signal);
    goal->contact_id = NULL;
    goal->description = NULL;
    goal->success_signal = NULL;
}
