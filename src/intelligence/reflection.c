#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/intelligence/reflection.h"
#include <stdio.h>
#include <string.h>

#define HU_REFLECTION_ESCAPE_BUF 1024
#define HU_REFLECTION_SQL_BUF 4096

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

hu_error_t hu_reflection_create_tables_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 1024)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS feedback_signals (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    contact_id TEXT NOT NULL,\n"
        "    type INTEGER NOT NULL,\n"
        "    context TEXT,\n"
        "    our_action TEXT,\n"
        "    timestamp INTEGER NOT NULL\n"
        ");\n"
        "CREATE INDEX IF NOT EXISTS idx_feedback_contact ON feedback_signals(contact_id);\n"
        "CREATE INDEX IF NOT EXISTS idx_feedback_timestamp ON feedback_signals(timestamp);\n"
        "CREATE TABLE IF NOT EXISTS reflections (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    period TEXT NOT NULL,\n"
        "    insights TEXT,\n"
        "    improvements TEXT,\n"
        "    created_at INTEGER NOT NULL\n"
        ");\n"
        "CREATE INDEX IF NOT EXISTS idx_reflections_period ON reflections(period);\n"
        "CREATE INDEX IF NOT EXISTS idx_reflections_created ON reflections(created_at);\n";

    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_feedback_insert_sql(const hu_feedback_signal_t *fb, char *buf, size_t cap,
                                 size_t *out_len) {
    if (!fb || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    if (!fb->contact_id)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_REFLECTION_ESCAPE_BUF];
    char context_esc[HU_REFLECTION_ESCAPE_BUF];
    char action_esc[HU_REFLECTION_ESCAPE_BUF];

    size_t ce_len, cxe_len, ae_len;
    escape_sql_string(fb->contact_id, fb->contact_id_len, contact_esc, sizeof(contact_esc),
                      &ce_len);
    escape_sql_string(fb->context ? fb->context : "", fb->context_len, context_esc,
                      sizeof(context_esc), &cxe_len);
    escape_sql_string(fb->our_action ? fb->our_action : "", fb->our_action_len, action_esc,
                      sizeof(action_esc), &ae_len);

    int n = snprintf(buf, cap,
                    "INSERT INTO feedback_signals (contact_id, type, context, our_action, timestamp) "
                    "VALUES ('%s', %d, '%s', '%s', %llu)",
                    contact_esc, (int)fb->type, context_esc, action_esc,
                    (unsigned long long)fb->timestamp);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_feedback_query_recent_sql(const char *contact_id, size_t len, uint32_t limit,
                                        char *buf, size_t cap, size_t *out_len) {
    if (!contact_id || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_REFLECTION_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id, len, contact_esc, sizeof(contact_esc), &ce_len);

    int n = snprintf(buf, cap,
                    "SELECT id, contact_id, type, context, our_action, timestamp FROM "
                    "feedback_signals WHERE contact_id = '%s' ORDER BY timestamp DESC LIMIT %u",
                    contact_esc, limit);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_reflection_insert_sql(const hu_reflection_entry_t *entry, char *buf, size_t cap,
                                   size_t *out_len) {
    if (!entry || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    if (!entry->period)
        return HU_ERR_INVALID_ARGUMENT;

    char period_esc[HU_REFLECTION_ESCAPE_BUF];
    char insights_esc[HU_REFLECTION_ESCAPE_BUF];
    char improvements_esc[HU_REFLECTION_ESCAPE_BUF];

    size_t pe_len, ie_len, ime_len;
    escape_sql_string(entry->period, entry->period_len, period_esc, sizeof(period_esc), &pe_len);
    escape_sql_string(entry->insights ? entry->insights : "", entry->insights_len, insights_esc,
                      sizeof(insights_esc), &ie_len);
    escape_sql_string(entry->improvements ? entry->improvements : "", entry->improvements_len,
                      improvements_esc, sizeof(improvements_esc), &ime_len);

    int n = snprintf(buf, cap,
                    "INSERT INTO reflections (period, insights, improvements, created_at) "
                    "VALUES ('%s', '%s', '%s', %llu)",
                    period_esc, insights_esc, improvements_esc,
                    (unsigned long long)entry->created_at);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_reflection_query_latest_sql(const char *period, size_t period_len, char *buf,
                                          size_t cap, size_t *out_len) {
    if (!period || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char period_esc[HU_REFLECTION_ESCAPE_BUF];
    size_t pe_len;
    escape_sql_string(period, period_len, period_esc, sizeof(period_esc), &pe_len);

    int n = snprintf(buf, cap,
                    "SELECT id, period, insights, improvements, created_at FROM reflections "
                    "WHERE period = '%s' ORDER BY created_at DESC LIMIT 1",
                    period_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_feedback_signal_type_t hu_feedback_classify(uint32_t response_time_seconds,
                                               size_t response_length, bool contains_question,
                                               bool left_on_read) {
    (void)response_time_seconds;
    if (left_on_read)
        return HU_FEEDBACK_NEGATIVE;
    if (response_length < 5)
        return HU_FEEDBACK_NEGATIVE;
    if (contains_question)
        return HU_FEEDBACK_POSITIVE;
    return HU_FEEDBACK_NEUTRAL;
}

double hu_skill_proficiency_score(uint32_t positive_count, uint32_t total_count,
                                 uint32_t practice_count) {
    if (total_count == 0)
        return 0.0;
    double ratio = (double)positive_count / (double)total_count;
    double practice_factor = 1.0;
    if (practice_count < 10)
        practice_factor = (double)practice_count / 10.0;
    double score = ratio * practice_factor;
    if (score > 1.0)
        return 1.0;
    if (score < 0.0)
        return 0.0;
    return score;
}

double hu_cross_contact_learning_weight(double source_proficiency, double relevance) {
    return source_proficiency * relevance * 0.5;
}

hu_error_t hu_reflection_build_prompt(hu_allocator_t *alloc,
                                     const hu_reflection_entry_t *latest,
                                     const hu_feedback_signal_t *recent_feedback, size_t fb_count,
                                     char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    uint32_t pos_count = 0;
    uint32_t neg_count = 0;
    uint32_t neu_count = 0;
    uint32_t corr_count = 0;
    for (size_t i = 0; i < fb_count && recent_feedback; i++) {
        switch (recent_feedback[i].type) {
        case HU_FEEDBACK_POSITIVE:
            pos_count++;
            break;
        case HU_FEEDBACK_NEGATIVE:
            neg_count++;
            break;
        case HU_FEEDBACK_NEUTRAL:
            neu_count++;
            break;
        case HU_FEEDBACK_CORRECTION:
            corr_count++;
            break;
        }
    }

    const char *insights_str = (latest && latest->insights && latest->insights_len > 0)
                                  ? latest->insights
                                  : "(none yet)";
    size_t insights_len =
        (latest && latest->insights && latest->insights_len > 0) ? latest->insights_len : 10;

    const char *focus = "ask more questions.";
    if (neg_count > pos_count)
        focus = "ask more questions.";
    else if (corr_count > 0)
        focus = "clarify intent before acting.";
    else if (pos_count > neg_count)
        focus = "continue current approach.";

    size_t cap = 512 + insights_len + 128;
    char *p = (char *)alloc->alloc(alloc->ctx, cap);
    if (!p)
        return HU_ERR_OUT_OF_MEMORY;

    int n = snprintf(p, cap,
                    "[RECENT LEARNING]: Last reflection: [%.*s]. Recent feedback: %u positive, "
                    "%u negative, %u neutral. Focus: %s",
                    (int)insights_len, insights_str, pos_count, neg_count, neu_count, focus);
    if (n < 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, p, cap);
        return HU_ERR_INVALID_ARGUMENT;
    }

    *out = p;
    *out_len = (size_t)n;
    return HU_OK;
}

const char *hu_feedback_type_str(hu_feedback_signal_type_t type) {
    switch (type) {
    case HU_FEEDBACK_POSITIVE:
        return "positive";
    case HU_FEEDBACK_NEGATIVE:
        return "negative";
    case HU_FEEDBACK_NEUTRAL:
        return "neutral";
    case HU_FEEDBACK_CORRECTION:
        return "correction";
    default:
        return "unknown";
    }
}

void hu_feedback_signal_deinit(hu_allocator_t *alloc, hu_feedback_signal_t *fb) {
    if (!alloc || !fb)
        return;
    if (fb->contact_id) {
        hu_str_free(alloc, fb->contact_id);
        fb->contact_id = NULL;
        fb->contact_id_len = 0;
    }
    if (fb->context) {
        hu_str_free(alloc, fb->context);
        fb->context = NULL;
        fb->context_len = 0;
    }
    if (fb->our_action) {
        hu_str_free(alloc, fb->our_action);
        fb->our_action = NULL;
        fb->our_action_len = 0;
    }
}

void hu_reflection_entry_deinit(hu_allocator_t *alloc, hu_reflection_entry_t *entry) {
    if (!alloc || !entry)
        return;
    if (entry->period) {
        hu_str_free(alloc, entry->period);
        entry->period = NULL;
        entry->period_len = 0;
    }
    if (entry->insights) {
        hu_str_free(alloc, entry->insights);
        entry->insights = NULL;
        entry->insights_len = 0;
    }
    if (entry->improvements) {
        hu_str_free(alloc, entry->improvements);
        entry->improvements = NULL;
        entry->improvements_len = 0;
    }
}

void hu_skill_observation_deinit(hu_allocator_t *alloc, hu_skill_observation_t *obs) {
    if (!alloc || !obs)
        return;
    if (obs->skill_name) {
        hu_str_free(alloc, obs->skill_name);
        obs->skill_name = NULL;
        obs->skill_name_len = 0;
    }
    if (obs->contact_id) {
        hu_str_free(alloc, obs->contact_id);
        obs->contact_id = NULL;
        obs->contact_id_len = 0;
    }
}
