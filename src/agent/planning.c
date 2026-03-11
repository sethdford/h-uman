#include "human/agent/planning.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

#define HU_PLANNING_ESCAPE_BUF 2048

/* Escape single quotes by doubling for SQL. Writes to out, returns length or 0 on overflow. */
static size_t escape_sql_string(const char *s, size_t len, char *out, size_t out_cap)
{
    size_t j = 0;
    for (size_t i = 0; i < len && s[i] != '\0'; i++) {
        if (s[i] == '\'') {
            if (j + 2 > out_cap)
                return 0;
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            if (j + 1 > out_cap)
                return 0;
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return j;
}

hu_error_t hu_planning_create_table_sql(char *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS plans (\n"
        "    id INTEGER PRIMARY KEY,\n"
        "    contact_id TEXT NOT NULL,\n"
        "    activity TEXT NOT NULL,\n"
        "    proposed_at INTEGER NOT NULL,\n"
        "    proposed_by_self INTEGER NOT NULL DEFAULT 1,\n"
        "    status TEXT DEFAULT 'proposed',\n"
        "    suggested_time TEXT,\n"
        "    scheduled_time INTEGER,\n"
        "    location TEXT,\n"
        "    reminder_sent INTEGER DEFAULT 0,\n"
        "    completed_at INTEGER,\n"
        "    outcome_notes TEXT\n"
        ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_planning_insert_sql(const hu_plan_t *plan, char *buf, size_t cap, size_t *out_len)
{
    if (!plan || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char esc_contact[HU_PLANNING_ESCAPE_BUF];
    char esc_activity[HU_PLANNING_ESCAPE_BUF];
    char esc_suggested[HU_PLANNING_ESCAPE_BUF];
    char esc_location[HU_PLANNING_ESCAPE_BUF];

    const char *contact = plan->contact_id ? plan->contact_id : "";
    size_t contact_len = plan->contact_id_len ? plan->contact_id_len : strlen(contact);
    if (escape_sql_string(contact, contact_len, esc_contact, sizeof(esc_contact)) == 0 &&
        contact_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *activity = plan->activity ? plan->activity : "";
    size_t activity_len = plan->activity_len ? plan->activity_len : strlen(activity);
    if (escape_sql_string(activity, activity_len, esc_activity, sizeof(esc_activity)) == 0 &&
        activity_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *suggested = plan->suggested_time ? plan->suggested_time : "";
    size_t suggested_len = plan->suggested_time_len ? plan->suggested_time_len : strlen(suggested);
    escape_sql_string(suggested, suggested_len, esc_suggested, sizeof(esc_suggested));

    const char *location = plan->location ? plan->location : "";
    size_t location_len = plan->location_len ? plan->location_len : strlen(location);
    escape_sql_string(location, location_len, esc_location, sizeof(esc_location));

    const char *status_str = hu_plan_status_str(plan->status);
    int n = snprintf(buf, cap,
                     "INSERT INTO plans (id, contact_id, activity, proposed_at, proposed_by_self, "
                     "status, suggested_time, scheduled_time, location, reminder_sent, completed_at, "
                     "outcome_notes) VALUES (%lld, '%s', '%s', %llu, %d, '%s', '%s', %llu, '%s', "
                     "%d, %llu, NULL)",
                     (long long)plan->id, esc_contact, esc_activity,
                     (unsigned long long)plan->proposed_at, plan->proposed_by_self ? 1 : 0,
                     status_str, esc_suggested, (unsigned long long)plan->scheduled_time,
                     esc_location, plan->reminder_sent ? 1 : 0,
                     (unsigned long long)plan->completed_at);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_planning_update_status_sql(int64_t plan_id, hu_plan_status_t new_status,
                                        char *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;
    const char *status_str = hu_plan_status_str(new_status);
    int n = snprintf(buf, cap, "UPDATE plans SET status = '%s' WHERE id = %lld",
                     status_str, (long long)plan_id);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_planning_query_sql(const char *contact_id, size_t contact_id_len,
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
                     "SELECT id, contact_id, activity, proposed_at, proposed_by_self, status, "
                     "suggested_time, scheduled_time, location, reminder_sent, completed_at "
                     "FROM plans WHERE contact_id = '%s' "
                     "ORDER BY proposed_at DESC LIMIT 20",
                     sanitized);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_planning_count_recent_sql(const char *contact_id, size_t contact_id_len,
                                        uint64_t since_ms,
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
                     "SELECT COUNT(*) FROM plans "
                     "WHERE contact_id = '%s' "
                     "AND proposed_by_self = 1 "
                     "AND proposed_at > %llu",
                     sanitized, (unsigned long long)since_ms);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

bool hu_planning_should_propose(const hu_planning_config_t *config,
                               uint8_t recent_proposal_count,
                               uint64_t last_rejection_ms,
                               uint64_t last_completed_ms,
                               uint64_t now_ms)
{
    (void)last_completed_ms;
    uint8_t max_proposals = 2;
    uint16_t cooldown_days = 14;
    if (config) {
        max_proposals = config->max_proposals_per_contact_per_month > 0
                           ? config->max_proposals_per_contact_per_month
                           : 2;
        cooldown_days = config->rejection_cooldown_days > 0 ? config->rejection_cooldown_days : 14;
    }

    if (recent_proposal_count >= max_proposals)
        return false;

    if (last_rejection_ms > 0) {
        uint64_t cooldown_ms = (uint64_t)cooldown_days * 86400000ULL;
        if (now_ms > last_rejection_ms && (now_ms - last_rejection_ms) < cooldown_ms)
            return false;
    }

    return true;
}

double hu_planning_score_proposal(bool shared_interest_match,
                                  bool good_weather,
                                  bool approaching_hangout_window,
                                  bool seasonal_event,
                                  double relationship_closeness)
{
    double score = 0.0;
    if (shared_interest_match)
        score += 0.3;
    if (good_weather)
        score += 0.1;
    if (approaching_hangout_window)
        score += 0.25;
    if (seasonal_event)
        score += 0.15;
    score += relationship_closeness * 0.2;

    if (score < 0.0)
        return 0.0;
    if (score > 1.0)
        return 1.0;
    return score;
}

hu_error_t hu_planning_build_context(hu_allocator_t *alloc,
                                    const hu_plan_t *plans, size_t plan_count,
                                    char **out, size_t *out_len)
{
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (!plans || plan_count == 0) {
        *out = hu_strndup(alloc, "[No active plans with this contact]", 35);
        if (!*out)
            return HU_ERR_OUT_OF_MEMORY;
        *out_len = 35;
        return HU_OK;
    }

    char buf[4096];
    size_t pos = 0;
    const char *hdr = "[ACTIVE PLANS with this contact]:\n";
    size_t hdr_len = strlen(hdr);
    if (pos + hdr_len >= sizeof(buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf + pos, hdr, hdr_len + 1);
    pos += hdr_len;

    for (size_t i = 0; i < plan_count && pos < sizeof(buf) - 1; i++) {
        const char *act = plans[i].activity ? plans[i].activity : "(no activity)";
        size_t act_len = plans[i].activity_len ? plans[i].activity_len : strlen(act);
        const char *status = hu_plan_status_str(plans[i].status);
        const char *sug = plans[i].suggested_time ? plans[i].suggested_time : "";
        size_t sug_len = plans[i].suggested_time_len ? plans[i].suggested_time_len : strlen(sug);
        const char *who = plans[i].proposed_by_self ? "we proposed" : "they proposed";

        int n = snprintf(buf + pos, sizeof(buf) - pos, "- %.*s (%s, %.*s) - %s\n",
                         (int)act_len, act, status, (int)sug_len, sug, who);
        if (n < 0 || (size_t)n >= sizeof(buf) - pos)
            break;
        pos += (size_t)n;
    }

    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}

const char *hu_plan_status_str(hu_plan_status_t status)
{
    switch (status) {
    case HU_PLAN_PROPOSED:
        return "proposed";
    case HU_PLAN_ACCEPTED:
        return "accepted";
    case HU_PLAN_CONFIRMED:
        return "confirmed";
    case HU_PLAN_COMPLETED:
        return "completed";
    case HU_PLAN_CANCELLED:
        return "cancelled";
    case HU_PLAN_DECLINED:
        return "declined";
    case HU_PLAN_EXPIRED:
        return "expired";
    default:
        return "proposed";
    }
}

bool hu_plan_status_from_str(const char *str, hu_plan_status_t *out)
{
    if (!str || !out)
        return false;
    if (strcmp(str, "proposed") == 0) {
        *out = HU_PLAN_PROPOSED;
        return true;
    }
    if (strcmp(str, "accepted") == 0) {
        *out = HU_PLAN_ACCEPTED;
        return true;
    }
    if (strcmp(str, "confirmed") == 0) {
        *out = HU_PLAN_CONFIRMED;
        return true;
    }
    if (strcmp(str, "completed") == 0) {
        *out = HU_PLAN_COMPLETED;
        return true;
    }
    if (strcmp(str, "cancelled") == 0) {
        *out = HU_PLAN_CANCELLED;
        return true;
    }
    if (strcmp(str, "declined") == 0) {
        *out = HU_PLAN_DECLINED;
        return true;
    }
    if (strcmp(str, "expired") == 0) {
        *out = HU_PLAN_EXPIRED;
        return true;
    }
    return false;
}

void hu_plan_deinit(hu_allocator_t *alloc, hu_plan_t *plan)
{
    if (!alloc || !plan)
        return;
    if (plan->contact_id)
        hu_str_free(alloc, plan->contact_id);
    if (plan->activity)
        hu_str_free(alloc, plan->activity);
    if (plan->suggested_time)
        hu_str_free(alloc, plan->suggested_time);
    if (plan->location)
        hu_str_free(alloc, plan->location);
    plan->contact_id = NULL;
    plan->activity = NULL;
    plan->suggested_time = NULL;
    plan->location = NULL;
}

void hu_plan_proposal_deinit(hu_allocator_t *alloc, hu_plan_proposal_t *proposal)
{
    if (!alloc || !proposal)
        return;
    if (proposal->activity)
        hu_str_free(alloc, proposal->activity);
    if (proposal->suggested_time)
        hu_str_free(alloc, proposal->suggested_time);
    if (proposal->reasoning)
        hu_str_free(alloc, proposal->reasoning);
    proposal->activity = NULL;
    proposal->suggested_time = NULL;
    proposal->reasoning = NULL;
}
