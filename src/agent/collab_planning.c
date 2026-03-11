#include "human/agent/collab_planning.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HU_COLLAB_ESCAPE_BUF 1024
#define HU_COLLAB_SQL_BUF 4096

static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap, size_t *out_len) {
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

static int tolower_char(unsigned char c) {
    return tolower(c);
}

static bool strncasecmp_eq(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        if (tolower_char((unsigned char)a[i]) != tolower_char((unsigned char)b[i]))
            return false;
    }
    return true;
}

/* Count word overlaps between reason and interests (case-insensitive). */
static size_t count_word_overlaps(const char *reason, size_t reason_len, const char **interests,
                                  size_t interest_count) {
    if (!reason || reason_len == 0 || !interests || interest_count == 0)
        return 0;

    size_t overlaps = 0;
    for (size_t i = 0; i < interest_count; i++) {
        const char *interest = interests[i];
        if (!interest)
            continue;
        size_t interest_len = strlen(interest);
        if (interest_len == 0)
            continue;
        if (reason_len >= interest_len &&
            strncasecmp_eq(reason, reason_len, interest, interest_len)) {
            overlaps++;
            continue;
        }
        /* Check if interest appears as substring/word in reason */
        for (size_t j = 0; j + interest_len <= reason_len; j++) {
            bool word_boundary_before = (j == 0) || !isalnum((unsigned char)reason[j - 1]);
            bool word_boundary_after =
                (j + interest_len >= reason_len) || !isalnum((unsigned char)reason[j + interest_len]);
            if (word_boundary_before && word_boundary_after &&
                strncasecmp_eq(reason + j, interest_len, interest, interest_len)) {
                overlaps++;
                break;
            }
        }
    }
    return overlaps;
}

hu_error_t hu_collab_plan_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS plans (\n"
        "    id INTEGER PRIMARY KEY,\n"
        "    contact_id TEXT NOT NULL,\n"
        "    description TEXT NOT NULL,\n"
        "    location TEXT,\n"
        "    proposed_time INTEGER NOT NULL,\n"
        "    status TEXT NOT NULL DEFAULT 'proposed',\n"
        "    trigger_type TEXT NOT NULL,\n"
        "    created_at INTEGER NOT NULL,\n"
        "    updated_at INTEGER NOT NULL\n"
        ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_collab_plan_insert_sql(const hu_collab_plan_t *plan, char *buf, size_t cap,
                                     size_t *out_len) {
    if (!plan || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    if (!plan->contact_id || !plan->description)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_COLLAB_ESCAPE_BUF];
    char desc_esc[HU_COLLAB_ESCAPE_BUF];
    char loc_esc[HU_COLLAB_ESCAPE_BUF];

    size_t ce_len, de_len, le_len;
    escape_sql_string(plan->contact_id, plan->contact_id_len, contact_esc, sizeof(contact_esc),
                      &ce_len);
    escape_sql_string(plan->description, plan->description_len, desc_esc, sizeof(desc_esc),
                      &de_len);

    const char *status_str = hu_plan_status_str(plan->status);
    const char *trigger_str = "interest_match";
    switch (plan->trigger_type) {
        case HU_PLAN_TRIGGER_INTEREST_MATCH:
            trigger_str = "interest_match";
            break;
        case HU_PLAN_TRIGGER_SEASONAL:
            trigger_str = "seasonal";
            break;
        case HU_PLAN_TRIGGER_EXTERNAL_EVENT:
            trigger_str = "external_event";
            break;
        case HU_PLAN_TRIGGER_MILESTONE:
            trigger_str = "milestone";
            break;
        case HU_PLAN_TRIGGER_SPONTANEOUS:
            trigger_str = "spontaneous";
            break;
        default:
            trigger_str = "interest_match";
            break;
    }

    uint64_t now_ms = plan->proposed_time_ms > 0 ? plan->proposed_time_ms : 1;

    int n;
    if (plan->location && plan->location_len > 0) {
        escape_sql_string(plan->location, plan->location_len, loc_esc, sizeof(loc_esc), &le_len);
        n = snprintf(buf, cap,
                     "INSERT INTO plans (id, contact_id, description, location, proposed_time, "
                     "status, trigger_type, created_at, updated_at) VALUES "
                     "(%lld, '%s', '%s', '%s', %llu, '%s', '%s', %llu, %llu)",
                     (long long)plan->plan_id, contact_esc, desc_esc, loc_esc,
                     (unsigned long long)plan->proposed_time_ms, status_str, trigger_str,
                     (unsigned long long)now_ms, (unsigned long long)now_ms);
    } else {
        n = snprintf(buf, cap,
                     "INSERT INTO plans (id, contact_id, description, location, proposed_time, "
                     "status, trigger_type, created_at, updated_at) VALUES "
                     "(%lld, '%s', '%s', NULL, %llu, '%s', '%s', %llu, %llu)",
                     (long long)plan->plan_id, contact_esc, desc_esc,
                     (unsigned long long)plan->proposed_time_ms, status_str, trigger_str,
                     (unsigned long long)now_ms, (unsigned long long)now_ms);
    }
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_collab_plan_query_sql(const char *contact_id, size_t contact_id_len,
                                    const hu_plan_status_t *status_filter, char *buf, size_t cap,
                                    size_t *out_len) {
    if (!contact_id || contact_id_len == 0 || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_COLLAB_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id, contact_id_len, contact_esc, sizeof(contact_esc), &ce_len);

    int n;
    if (status_filter != NULL && (int)*status_filter <= HU_PLAN_DECLINED) {
        const char *status_str = hu_plan_status_str(*status_filter);
        n = snprintf(buf, cap,
                     "SELECT id, contact_id, description, location, proposed_time, status, "
                     "trigger_type, created_at, updated_at FROM plans "
                     "WHERE contact_id = '%s' AND status = '%s' ORDER BY proposed_time DESC",
                     contact_esc, status_str);
    } else {
        n = snprintf(buf, cap,
                     "SELECT id, contact_id, description, location, proposed_time, status, "
                     "trigger_type, created_at, updated_at FROM plans "
                     "WHERE contact_id = '%s' ORDER BY proposed_time DESC",
                     contact_esc);
    }
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_collab_plan_update_status_sql(int64_t plan_id, hu_plan_status_t new_status,
                                            char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;
    if ((int)new_status > HU_PLAN_DECLINED)
        return HU_ERR_INVALID_ARGUMENT;

    const char *status_str = hu_plan_status_str(new_status);
    int n = snprintf(buf, cap,
                     "UPDATE plans SET status = '%s', updated_at = strftime('%%s', 'now') * 1000 "
                     "WHERE id = %lld",
                     status_str, (long long)plan_id);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

double hu_collab_plan_score_trigger(const hu_plan_trigger_t *trigger, const char **interests,
                                    size_t interest_count) {
    if (!trigger || !trigger->reason || trigger->reason_len == 0)
        return 0.0;
    if (!interests || interest_count == 0)
        return trigger->relevance_score;

    size_t overlaps = count_word_overlaps(trigger->reason, trigger->reason_len, interests,
                                         interest_count);
    if (overlaps == 0)
        return 0.0;

    double match_score = (double)overlaps / (double)interest_count;
    if (match_score > 1.0)
        match_score = 1.0;
    return match_score * 0.7 + trigger->relevance_score * 0.3;
}

bool hu_collab_plan_should_propose(const char *contact_id, size_t contact_id_len,
                                   unsigned int daily_proposals_sent, unsigned int max_daily,
                                   double relationship_closeness) {
    (void)contact_id;
    (void)contact_id_len;
    if (daily_proposals_sent >= max_daily)
        return false;
    if (relationship_closeness <= 0.4)
        return false;
    return true;
}

static const char *trigger_type_str(hu_plan_trigger_type_t t) {
    switch (t) {
        case HU_PLAN_TRIGGER_INTEREST_MATCH:
            return "interest_match";
        case HU_PLAN_TRIGGER_SEASONAL:
            return "seasonal";
        case HU_PLAN_TRIGGER_EXTERNAL_EVENT:
            return "external_event";
        case HU_PLAN_TRIGGER_MILESTONE:
            return "milestone";
        case HU_PLAN_TRIGGER_SPONTANEOUS:
            return "spontaneous";
        default:
            return "interest_match";
    }
}

hu_error_t hu_collab_plan_build_prompt(hu_allocator_t *alloc,
                                      const hu_plan_trigger_t *triggers, size_t trigger_count,
                                      const hu_collab_plan_t *recent_plans, size_t plan_count,
                                      char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *header = "[COLLABORATIVE PLANNING CONTEXT]\n";
    char *result = hu_strndup(alloc, header, strlen(header));
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    size_t result_len = strlen(header);

    if (trigger_count > 0 && triggers) {
        const char *tr_header = "\nTriggers to consider:\n";
        char *new_result = hu_sprintf(alloc, "%.*s%s", (int)result_len, result, tr_header);
        hu_str_free(alloc, result);
        if (!new_result)
            return HU_ERR_OUT_OF_MEMORY;
        result = new_result;
        result_len = strlen(result);

        for (size_t i = 0; i < trigger_count; i++) {
            const hu_plan_trigger_t *t = &triggers[i];
            const char *reason = t->reason ? t->reason : "";
            size_t reason_len = t->reason_len;
            char line[512];
            int n = snprintf(line, sizeof(line), "- %s (%.*s) score=%.2f\n",
                             trigger_type_str(t->trigger_type), (int)reason_len, reason,
                             t->relevance_score);
            if (n < 0 || (size_t)n >= sizeof(line))
                continue;
            new_result = hu_sprintf(alloc, "%.*s%s", (int)result_len, result, line);
            hu_str_free(alloc, result);
            if (!new_result)
                return HU_ERR_OUT_OF_MEMORY;
            result = new_result;
            result_len = strlen(result);
        }
    }

    if (plan_count > 0 && recent_plans) {
        const char *pl_header = "\nRecent plans:\n";
        char *new_result = hu_sprintf(alloc, "%.*s%s", (int)result_len, result, pl_header);
        hu_str_free(alloc, result);
        if (!new_result)
            return HU_ERR_OUT_OF_MEMORY;
        result = new_result;
        result_len = strlen(result);

        for (size_t i = 0; i < plan_count; i++) {
            const hu_collab_plan_t *p = &recent_plans[i];
            const char *desc = p->description ? p->description : "";
            size_t desc_len = p->description_len;
            const char *status_str = hu_plan_status_str(p->status);
            char line[512];
            int n = snprintf(line, sizeof(line), "- %.*s [%s]\n", (int)desc_len, desc, status_str);
            if (n < 0 || (size_t)n >= sizeof(line))
                continue;
            new_result = hu_sprintf(alloc, "%.*s%s", (int)result_len, result, line);
            hu_str_free(alloc, result);
            if (!new_result)
                return HU_ERR_OUT_OF_MEMORY;
            result = new_result;
            result_len = strlen(result);
        }
    }

    if (trigger_count == 0 && plan_count == 0) {
        const char *empty = "\nNo triggers or recent plans.";
        char *new_result = hu_sprintf(alloc, "%.*s%s", (int)result_len, result, empty);
        hu_str_free(alloc, result);
        if (!new_result)
            return HU_ERR_OUT_OF_MEMORY;
        result = new_result;
    }

    *out = result;
    *out_len = strlen(result);
    return HU_OK;
}

void hu_collab_plan_deinit(hu_allocator_t *alloc, hu_collab_plan_t *plan) {
    if (!alloc || !plan)
        return;
    if (plan->contact_id) {
        hu_str_free(alloc, plan->contact_id);
        plan->contact_id = NULL;
        plan->contact_id_len = 0;
    }
    if (plan->description) {
        hu_str_free(alloc, plan->description);
        plan->description = NULL;
        plan->description_len = 0;
    }
    if (plan->location) {
        hu_str_free(alloc, plan->location);
        plan->location = NULL;
        plan->location_len = 0;
    }
}

void hu_plan_trigger_deinit(hu_allocator_t *alloc, hu_plan_trigger_t *trigger) {
    if (!alloc || !trigger)
        return;
    if (trigger->contact_id) {
        hu_str_free(alloc, trigger->contact_id);
        trigger->contact_id = NULL;
        trigger->contact_id_len = 0;
    }
    if (trigger->reason) {
        hu_str_free(alloc, trigger->reason);
        trigger->reason = NULL;
        trigger->reason_len = 0;
    }
}

/* hu_plan_status_str is defined in planning.c — reuse via planning.h */
