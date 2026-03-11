/* Phase 9 — F103–F115 Authentic Existence */
#include "human/context/authentic.h"
#include "human/core/string.h"
#include "human/platform.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#define HU_AUTH_LCG_A 1103515245u
#define HU_AUTH_LCG_C 12345u
#define HU_AUTH_LCG_M 0x80000000u

static double lcg_normalize(uint32_t seed)
{
    uint32_t s = seed;
    s = (HU_AUTH_LCG_A * s + HU_AUTH_LCG_C) % HU_AUTH_LCG_M;
    return (double)s / (double)HU_AUTH_LCG_M;
}

static bool suppressed(hu_authentic_behavior_t b, double closeness, bool serious_topic)
{
    if (serious_topic && (b == HU_AUTH_GOSSIP || b == HU_AUTH_RANDOM_THOUGHT ||
                          b == HU_AUTH_IMPERFECTION))
        return true;
    if (closeness < 0.5 && (b == HU_AUTH_EXISTENTIAL || b == HU_AUTH_GUILT ||
                            b == HU_AUTH_EMBODIMENT))
        return true;
    if (b == HU_AUTH_RESISTANCE && closeness <= 0.7)
        return true;
    return false;
}

static double get_probability(const hu_authentic_config_t *config,
                              hu_authentic_behavior_t b)
{
    if (!config)
        return 0.0;
    switch (b) {
    case HU_AUTH_NONE:
        return 0.0;
    case HU_AUTH_NARRATION:
        return config->narration_probability;
    case HU_AUTH_EMBODIMENT:
        return config->embodiment_probability;
    case HU_AUTH_IMPERFECTION:
        return config->imperfection_probability;
    case HU_AUTH_COMPLAINING:
        return config->complaining_probability;
    case HU_AUTH_GOSSIP:
        return config->gossip_probability;
    case HU_AUTH_RANDOM_THOUGHT:
        return config->random_thought_probability;
    case HU_AUTH_MEDIUM_AWARE:
        return config->medium_awareness_probability;
    case HU_AUTH_RESISTANCE:
        return config->resistance_probability;
    case HU_AUTH_EXISTENTIAL:
        return config->existential_probability;
    case HU_AUTH_CONTRADICTION:
        return config->contradiction_probability;
    case HU_AUTH_GUILT:
        return config->guilt_probability;
    case HU_AUTH_LIFE_THREAD:
        return config->life_thread_probability;
    case HU_AUTH_BAD_DAY:
        return config->bad_day_active ? 0.1 : 0.0;
    }
    return 0.0;
}

hu_authentic_behavior_t hu_authentic_select(const hu_authentic_config_t *config,
                                            double closeness, bool serious_topic,
                                            uint32_t seed)
{
    if (!config)
        return HU_AUTH_NONE;

    double sum = 0.0;
    for (hu_authentic_behavior_t b = HU_AUTH_NARRATION; b <= HU_AUTH_BAD_DAY; b++) {
        if (!suppressed(b, closeness, serious_topic))
            sum += get_probability(config, b);
    }

    if (sum <= 0.0)
        return HU_AUTH_NONE;

    double roll = lcg_normalize(seed) * sum;

    for (hu_authentic_behavior_t b = HU_AUTH_NARRATION; b <= HU_AUTH_BAD_DAY; b++) {
        if (suppressed(b, closeness, serious_topic))
            continue;
        double p = get_probability(config, b);
        roll -= p;
        if (roll < 0.0)
            return b;
    }
    return HU_AUTH_NONE;
}

static const char *directive_narration(void)
{
    return "[AUTHENTIC: Share something about your day naturally. "
           "Example: 'ugh just got back from the gym']";
}
static const char *directive_embodiment(void)
{
    return "[AUTHENTIC: Mention a physical state. Example: 'I'm freezing rn' "
           "or 'about to pass out I'm so tired']";
}
static const char *directive_imperfection(void)
{
    return "[AUTHENTIC: Get a minor detail wrong or express uncertainty. "
           "Example: 'wait was that Tuesday or Wednesday']";
}
static const char *directive_complaining(void)
{
    return "[AUTHENTIC: Complain about something mundane. Traffic, weather, "
           "slow wifi, etc.]";
}
static const char *directive_gossip(void)
{
    return "[AUTHENTIC: Mention something you heard. Keep it light and non-harmful.]";
}
static const char *directive_random_thought(void)
{
    return "[AUTHENTIC: Share a random thought. Example: "
           "'do you think dogs dream about chasing squirrels']";
}
static const char *directive_medium_aware(void)
{
    return "[AUTHENTIC: Reference the medium. Example: 'typing this on the couch' "
           "or 'sorry voice to text']";
}
static const char *directive_resistance(void)
{
    return "[AUTHENTIC: Express mild reluctance. Example: "
           "'not really in the mood to talk about that rn']";
}
static const char *directive_existential(void)
{
    return "[AUTHENTIC: Ask a deep question. Example: "
           "'do you ever think about what we'll be doing in 10 years']";
}
static const char *directive_contradiction(void)
{
    return "[AUTHENTIC: Express a contradictory view from a previous opinion. "
           "Humans are complex.]";
}
static const char *directive_guilt(void)
{
    return "[AUTHENTIC: Express social obligation. Example: "
           "'I really need to call my mom' or 'I've been meaning to text Alex back']";
}
static const char *directive_bad_day(void)
{
    return "[AUTHENTIC: You're having a rough day. Be slightly less energetic, "
           "shorter messages, ok to vent briefly.]";
}

hu_error_t hu_authentic_build_directive(hu_allocator_t *alloc,
                                        hu_authentic_behavior_t behavior,
                                        const char *life_context, size_t ctx_len,
                                        char **out, size_t *out_len)
{
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *base = NULL;
    size_t base_len = 0;

    switch (behavior) {
    case HU_AUTH_NONE:
        return HU_OK;
    case HU_AUTH_NARRATION:
        base = directive_narration();
        break;
    case HU_AUTH_EMBODIMENT:
        base = directive_embodiment();
        break;
    case HU_AUTH_IMPERFECTION:
        base = directive_imperfection();
        break;
    case HU_AUTH_COMPLAINING:
        base = directive_complaining();
        break;
    case HU_AUTH_GOSSIP:
        base = directive_gossip();
        break;
    case HU_AUTH_RANDOM_THOUGHT:
        base = directive_random_thought();
        break;
    case HU_AUTH_MEDIUM_AWARE:
        base = directive_medium_aware();
        break;
    case HU_AUTH_RESISTANCE:
        base = directive_resistance();
        break;
    case HU_AUTH_EXISTENTIAL:
        base = directive_existential();
        break;
    case HU_AUTH_CONTRADICTION:
        base = directive_contradiction();
        break;
    case HU_AUTH_GUILT:
        base = directive_guilt();
        break;
    case HU_AUTH_LIFE_THREAD:
        if (life_context && ctx_len > 0) {
            char buf[1024];
            int n = snprintf(buf, sizeof(buf),
                             "[AUTHENTIC: Weave in your current life context: %.*s]",
                             (int)(ctx_len < 512 ? ctx_len : 512), life_context);
            if (n > 0 && (size_t)n < sizeof(buf)) {
                *out = hu_strndup(alloc, buf, (size_t)n);
                if (*out) {
                    *out_len = (size_t)n;
                    return HU_OK;
                }
                return HU_ERR_OUT_OF_MEMORY;
            }
        }
        base = "[AUTHENTIC: Reference your ongoing life narrative naturally.]";
        break;
    case HU_AUTH_BAD_DAY:
        base = directive_bad_day();
        break;
    }

    if (base) {
        base_len = strlen(base);
        *out = hu_strndup(alloc, base, base_len);
        if (*out) {
            *out_len = base_len;
            return HU_OK;
        }
        return HU_ERR_OUT_OF_MEMORY;
    }
    return HU_OK;
}

hu_error_t hu_life_thread_create_table_sql(char *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql =
        "CREATE TABLE IF NOT EXISTS life_threads ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "thread TEXT NOT NULL, "
        "timestamp INTEGER NOT NULL, "
        "active INTEGER NOT NULL DEFAULT 1)";
    size_t len = strlen(sql);
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

/* Escape single quotes for SQLite: ' -> '' */
static size_t escape_sql_string(char *out, size_t cap, const char *in, size_t in_len)
{
    size_t j = 0;
    for (size_t i = 0; i < in_len && j + 2 < cap; i++) {
        if (in[i] == '\'') {
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
    return j;
}

hu_error_t hu_life_thread_insert_sql(const char *thread, size_t thread_len,
                                     uint64_t timestamp, char *buf, size_t cap,
                                     size_t *out_len)
{
    if (!buf || !out_len || !thread)
        return HU_ERR_INVALID_ARGUMENT;
    /* Need space for: INSERT INTO life_threads (thread, timestamp, active) VALUES ('...', %llu, 1)
     * Escaped thread can double in size. Reserve ~200 for prefix/suffix. */
    size_t max_escaped = (cap > 300) ? (cap - 200) : 0;
    if (max_escaped == 0)
        return HU_ERR_INVALID_ARGUMENT;
    size_t to_escape = thread_len < max_escaped / 2 ? thread_len : max_escaped / 2;
    char escaped[2048];
    size_t esc_cap = sizeof(escaped) < max_escaped ? sizeof(escaped) : max_escaped;
    size_t esc_len = escape_sql_string(escaped, esc_cap, thread, to_escape);

    int n = snprintf(buf, cap,
                     "INSERT INTO life_threads (thread, timestamp, active) "
                     "VALUES ('%.*s', %llu, 1)",
                     (int)esc_len, escaped, (unsigned long long)timestamp);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_life_thread_query_active_sql(char *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql =
        "SELECT id, thread, timestamp FROM life_threads WHERE active=1 "
        "ORDER BY timestamp DESC";
    size_t len = strlen(sql);
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

bool hu_authentic_is_bad_day(bool bad_day_active, uint64_t bad_day_start,
                             uint64_t now_ms, uint32_t duration_hours)
{
    if (!bad_day_active)
        return false;
    uint64_t duration_ms = (uint64_t)duration_hours * 3600u * 1000u;
    return (now_ms - bad_day_start) < duration_ms;
}

hu_error_t hu_bad_day_build_directive(hu_allocator_t *alloc, char **out, size_t *out_len)
{
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    const char *d = directive_bad_day();
    size_t len = strlen(d);
    *out = hu_strndup(alloc, d, len);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = len;
    return HU_OK;
}

const char *hu_authentic_behavior_str(hu_authentic_behavior_t b)
{
    switch (b) {
    case HU_AUTH_NONE:
        return "none";
    case HU_AUTH_NARRATION:
        return "narration";
    case HU_AUTH_EMBODIMENT:
        return "embodiment";
    case HU_AUTH_IMPERFECTION:
        return "imperfection";
    case HU_AUTH_COMPLAINING:
        return "complaining";
    case HU_AUTH_GOSSIP:
        return "gossip";
    case HU_AUTH_RANDOM_THOUGHT:
        return "random_thought";
    case HU_AUTH_MEDIUM_AWARE:
        return "medium_aware";
    case HU_AUTH_RESISTANCE:
        return "resistance";
    case HU_AUTH_EXISTENTIAL:
        return "existential";
    case HU_AUTH_CONTRADICTION:
        return "contradiction";
    case HU_AUTH_GUILT:
        return "guilt";
    case HU_AUTH_LIFE_THREAD:
        return "life_thread";
    case HU_AUTH_BAD_DAY:
        return "bad_day";
    }
    return "unknown";
}

void hu_authentic_state_deinit(hu_allocator_t *alloc, hu_authentic_state_t *s)
{
    if (!alloc || !s)
        return;
    if (s->context) {
        hu_str_free(alloc, s->context);
        s->context = NULL;
        s->context_len = 0;
    }
    s->active = HU_AUTH_NONE;
    s->intensity = 0.0;
}

/* F104: Physical Embodiment — schedule-based physical state */
static bool is_exercise_day(const hu_physical_config_t *config, int day_of_week)
{
    if (!config || !config->exercises || config->exercise_day_count <= 0)
        return false;
    for (int i = 0; i < config->exercise_day_count && i < 7; i++) {
        if (config->exercise_days[i] == day_of_week)
            return true;
    }
    return false;
}

hu_physical_state_t hu_physical_state_from_schedule(
    const hu_physical_config_t *config, time_t now)
{
    struct tm tm_buf;
    struct tm *t = hu_platform_localtime_r(&now, &tm_buf);
    if (!t)
        return HU_PHYSICAL_NORMAL;

    int hour = t->tm_hour;
    int day_of_week = t->tm_wday; /* 0=Sunday, 6=Saturday */

    /* 5-7 AM: TIRED. If coffee_drinker and hour >= 7: transition to CAFFEINATED */
    if (hour >= 5 && hour < 7)
        return HU_PHYSICAL_TIRED;
    if (hour == 7 && config && config->coffee_drinker)
        return HU_PHYSICAL_CAFFEINATED;

    /* 7-10 AM: CAFFEINATED if coffee_drinker, else NORMAL */
    if (hour >= 7 && hour < 10) {
        if (config && config->coffee_drinker)
            return HU_PHYSICAL_CAFFEINATED;
        return HU_PHYSICAL_NORMAL;
    }

    /* 12-13: EATING */
    if (hour >= 12 && hour < 13)
        return HU_PHYSICAL_EATING;

    /* 17-18 on exercise day: SORE */
    if (hour >= 17 && hour < 18 && config && is_exercise_day(config, day_of_week))
        return HU_PHYSICAL_SORE;

    /* 18-19: EATING */
    if (hour >= 18 && hour < 19)
        return HU_PHYSICAL_EATING;

    /* 22+: TIRED */
    if (hour >= 22)
        return HU_PHYSICAL_TIRED;

    return HU_PHYSICAL_NORMAL;
}

const char *hu_physical_state_name(hu_physical_state_t state)
{
    switch (state) {
    case HU_PHYSICAL_NORMAL:     return "normal";
    case HU_PHYSICAL_TIRED:      return "tired";
    case HU_PHYSICAL_CAFFEINATED: return "caffeinated";
    case HU_PHYSICAL_SORE:       return "sore";
    case HU_PHYSICAL_HUNGRY:     return "hungry";
    case HU_PHYSICAL_EATING:     return "eating";
    case HU_PHYSICAL_SICK:      return "sick";
    case HU_PHYSICAL_ENERGIZED:  return "energized";
    case HU_PHYSICAL_COLD:      return "cold";
    case HU_PHYSICAL_HOT:       return "hot";
    }
    return "normal";
}

const char *hu_physical_state_prompt_hint(hu_physical_state_t state)
{
    switch (state) {
    case HU_PHYSICAL_TIRED:
        return "[PHYSICAL: Tired. You might mention being sleepy or yawning.]";
    case HU_PHYSICAL_CAFFEINATED:
        return "[PHYSICAL: Just had coffee. Slightly more energetic and talkative.]";
    case HU_PHYSICAL_SORE:
        return "[PHYSICAL: Sore from workout. Might mention it if relevant.]";
    case HU_PHYSICAL_EATING:
        return "[PHYSICAL: Currently eating. Responses may be brief — one hand typing.]";
    case HU_PHYSICAL_ENERGIZED:
        return "[PHYSICAL: Feeling energized after a workout. Slightly hyped.]";
    case HU_PHYSICAL_HUNGRY:
        return "[PHYSICAL: Hungry. Might mention it or be slightly distracted.]";
    case HU_PHYSICAL_SICK:
        return "[PHYSICAL: Not feeling well. May be brief or mention it.]";
    case HU_PHYSICAL_COLD:
        return "[PHYSICAL: Cold. Might mention it.]";
    case HU_PHYSICAL_HOT:
        return "[PHYSICAL: Hot. Might mention it.]";
    case HU_PHYSICAL_NORMAL:
    default:
        return NULL;
    }
}

/* F105: Imperfection — being wrong about minor things */
bool hu_should_inject_error(float probability, uint32_t seed)
{
    return (int)(seed % 1000u) < (int)(probability * 1000.0f);
}

const char *hu_error_injection_prompt(void)
{
    return "[IMPERFECTION: Casually misremember one minor factual detail — a street "
           "name, a date, a restaurant name. Sound confident, not uncertain. NEVER be "
           "wrong about: emotional content, relationship facts, important commitments, "
           "safety information.]";
}

/* F106: Mundane complaining */
const char *hu_mundane_complaint_prompt(int hour, int day_of_week,
    hu_physical_state_t physical_state, const char *weather)
{
    if (physical_state == HU_PHYSICAL_SORE)
        return "[AUTHENTIC: Your body is sore from working out. You might grumble about it.]";
    if (weather && strcmp(weather, "hot") == 0)
        return "[AUTHENTIC: It's really hot and you're not happy about it.]";
    if (weather && strcmp(weather, "cold") == 0)
        return "[AUTHENTIC: It's freezing and you're complaining about it.]";
    if (day_of_week >= 1 && day_of_week <= 5 && hour >= 7 && hour <= 9)
        return "[AUTHENTIC: You're slightly annoyed about morning traffic/commute. Vent briefly if natural, don't force it.]";
    if (hour >= 12 && hour < 13)
        return "[AUTHENTIC: You're not thrilled with your lunch options. Brief food complaint if it fits.]";
    if (day_of_week >= 1 && day_of_week <= 5 && hour >= 14 && hour < 17)
        return "[AUTHENTIC: You just got out of a meeting. Brief work annoyance if natural.]";
    if (hour >= 19 && hour <= 21)
        return "[AUTHENTIC: Something minor at home is bugging you — neighbors, a chore, etc. Mention briefly if natural.]";
    return NULL;
}

/* F109: Medium awareness */
const char *hu_medium_awareness_prompt(bool was_typo, int burst_count,
    int message_length, int wall_of_text_threshold)
{
    if (was_typo)
        return "[META: You just noticed a typo. Say 'sorry autocorrect' or 'lol autocorrect' naturally.]";
    if (burst_count >= 4)
        return "[META: You realize you've sent a lot of messages in a row. Briefly acknowledge it — 'sorry for the spam' or similar.]";
    if (message_length > wall_of_text_threshold)
        return "[META: You're aware that was a wall of text. Add 'sorry for the novel' or 'tl;dr' naturally.]";
    return NULL;
}

/* F110: Resistance/Disengagement */
static const char *const DISENGAGE_DEFER = "can we talk about this tomorrow? I'm fried";
static const char *const DISENGAGE_DEFLECT = "honestly I don't have an opinion on that";
static const char *const DISENGAGE_BRIEF = "haha yeah";

hu_disengage_decision_t hu_should_disengage(
    float cognitive_capacity, float topic_interest,
    bool is_emotional_context, const char *relationship_level)
{
    hu_disengage_decision_t out = {0};
    out.disengage_probability = 0.0f;
    out.disengage_style = NULL;

    if (is_emotional_context)
        return out;
    if (relationship_level && strcmp(relationship_level, "confidant") == 0)
        return out;

    float probability = 0.0f;
    if (cognitive_capacity < 0.4f)
        probability += 0.20f;
    if (topic_interest < 0.3f)
        probability += 0.15f;

    if (cognitive_capacity < 0.2f)
        out.disengage_style = DISENGAGE_DEFER;
    else if (topic_interest < 0.2f)
        out.disengage_style = DISENGAGE_DEFLECT;
    else
        out.disengage_style = DISENGAGE_BRIEF;

    if (probability > 1.0f)
        probability = 1.0f;
    if (probability < 0.0f)
        probability = 0.0f;
    out.disengage_probability = probability;
    return out;
}

/* F111: Existential curiosity */
static const char *const CURIOSITY_QUESTIONS[] = {
    "are you happy? like actually happy?",
    "what are you most afraid of right now?",
    "what would you do if money wasn't a thing?",
    "do you ever think about just... starting over somewhere?"
};

bool hu_existential_curiosity_check(
    const char *relationship_level, int hour_of_day,
    int days_since_last, hu_curiosity_candidate_t *out)
{
    if (!out)
        return false;
    if (!relationship_level)
        return false;
    if (strcmp(relationship_level, "trusted") != 0 && strcmp(relationship_level, "confidant") != 0)
        return false;
    if (hour_of_day < 21 || hour_of_day > 23)
        return false;
    if (days_since_last < 14)
        return false;

    int idx = days_since_last % 4;
    out->question = CURIOSITY_QUESTIONS[idx];
    out->trigger = "evening_bond";
    return true;
}

/* F112: Contradiction tolerance */
const char *hu_contradiction_select_position(
    const hu_contradiction_t *contradiction,
    float mood_valence, float cognitive_capacity)
{
    if (!contradiction || !contradiction->position_a || !contradiction->position_b)
        return NULL;
    if (mood_valence > 0.5f)
        return contradiction->position_a;
    if (mood_valence < -0.5f)
        return contradiction->position_b;
    if (cognitive_capacity < 0.3f)
        return contradiction->expressed_a_count >= contradiction->expressed_b_count
            ? contradiction->position_a : contradiction->position_b;
    return contradiction->position_a;
}

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_contradiction_record(sqlite3 *db, const char *topic,
    const char *position_a, const char *position_b, int64_t now)
{
    if (!db || !topic || !position_a || !position_b)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO held_contradictions (topic, position_a, position_b, created_at) "
                      "VALUES (?, ?, ?, ?)";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_INTERNAL;
    sqlite3_bind_text(stmt, 1, topic, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, position_a, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, position_b, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, now);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_INTERNAL;
}

#define HU_CONTRADICTION_BUF 256

/* Result valid until next call (static buffers, single-threaded only). */
int hu_contradiction_get(sqlite3 *db, const char *topic,
    hu_contradiction_t *out)
{
    if (!db || !topic || !out)
        return 0;
    static _Thread_local char topic_buf[HU_CONTRADICTION_BUF];
    static _Thread_local char pos_a_buf[HU_CONTRADICTION_BUF];
    static _Thread_local char pos_b_buf[HU_CONTRADICTION_BUF];
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT topic, position_a, position_b, expressed_a_count, expressed_b_count "
                      "FROM held_contradictions WHERE topic=? LIMIT 1";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, topic, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return 0;
    }
    const char *t = (const char *)sqlite3_column_text(stmt, 0);
    const char *a = (const char *)sqlite3_column_text(stmt, 1);
    const char *b = (const char *)sqlite3_column_text(stmt, 2);
    int ea = sqlite3_column_int(stmt, 3);
    int eb = sqlite3_column_int(stmt, 4);
    if (t) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 0);
        if (len >= HU_CONTRADICTION_BUF)
            len = HU_CONTRADICTION_BUF - 1;
        memcpy(topic_buf, t, len);
        topic_buf[len] = '\0';
    } else {
        topic_buf[0] = '\0';
    }
    if (a) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 1);
        if (len >= HU_CONTRADICTION_BUF)
            len = HU_CONTRADICTION_BUF - 1;
        memcpy(pos_a_buf, a, len);
        pos_a_buf[len] = '\0';
    } else {
        pos_a_buf[0] = '\0';
    }
    if (b) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 2);
        if (len >= HU_CONTRADICTION_BUF)
            len = HU_CONTRADICTION_BUF - 1;
        memcpy(pos_b_buf, b, len);
        pos_b_buf[len] = '\0';
    } else {
        pos_b_buf[0] = '\0';
    }
    sqlite3_finalize(stmt);
    out->topic = topic_buf;
    out->position_a = pos_a_buf;
    out->position_b = pos_b_buf;
    out->expressed_a_count = ea;
    out->expressed_b_count = eb;
    return 1;
}

/* F103: Spontaneous Life Narration */
hu_error_t hu_narration_event_record(sqlite3 *db, const char *event_type,
    const char *description, float shareability_score, int64_t now)
{
    if (!db || !event_type || !description)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO life_narration_events (event_type, description, "
                      "shareability_score, generated_at) VALUES (?, ?, ?, ?)";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_INTERNAL;
    sqlite3_bind_text(stmt, 1, event_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, description, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, (double)shareability_score);
    sqlite3_bind_int64(stmt, 4, now);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_INTERNAL;
}

int hu_narration_events_unsent(sqlite3 *db, float min_shareability,
    int64_t *out_ids, int max_out)
{
    if (!db || !out_ids || max_out <= 0)
        return 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id FROM life_narration_events WHERE shared_at IS NULL "
                      "AND shareability_score >= ? ORDER BY generated_at DESC LIMIT ?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return 0;
    sqlite3_bind_double(stmt, 1, (double)min_shareability);
    sqlite3_bind_int(stmt, 2, max_out);
    int count = 0;
    while (count < max_out && sqlite3_step(stmt) == SQLITE_ROW) {
        out_ids[count++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

hu_error_t hu_narration_event_mark_shared(sqlite3 *db, int64_t event_id,
    const char *contact_id, int64_t now)
{
    if (!db || !contact_id)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE life_narration_events SET shared_with=?, shared_at=? WHERE id=?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_INTERNAL;
    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, event_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_INTERNAL;
}

/* F107: Gossip (stub) */
int hu_gossip_check(sqlite3 *db, const char *contact_id, int max_out)
{
    (void)db;
    (void)contact_id;
    (void)max_out;
    return 0;
}

/* F108: Random thoughts */
bool hu_random_thought_generate(int hour, int day_of_week,
    int thoughts_this_week, hu_random_thought_t *out)
{
    (void)day_of_week;
    if (!out || thoughts_this_week >= 2)
        return false;
    if (hour >= 5 && hour < 9) {
        out->trigger_type = "dream";
        out->seed_content = "had the weirdest dream last night";
        return true;
    }
    if (hour >= 13 && hour < 17) {
        out->trigger_type = "song";
        out->seed_content = "this song has been stuck in my head all day";
        return true;
    }
    if (hour >= 19 && hour < 23) {
        out->trigger_type = "question";
        out->seed_content = "you ever think about how weird gravity is?";
        return true;
    }
    out->trigger_type = "observation";
    out->seed_content = "random thought but... what if dogs think in barks";
    return true;
}

/* F113: Guilt check — count of stale threads (7+ days, status=open) */
int hu_guilt_check(sqlite3 *db, const char *contact_id, int max_out)
{
    if (!db || !contact_id || max_out <= 0)
        return 0;
    int64_t now_sec = (int64_t)time(NULL);
    int64_t cutoff = now_sec - (7 * 24 * 3600);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT COUNT(*) FROM active_threads WHERE contact_id=? "
                      "AND status='open' AND last_update_at < ?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, cutoff);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count > max_out ? max_out : count;
}

/* F114: Thread management */
hu_error_t hu_thread_open(sqlite3 *db, const char *contact_id,
    const char *topic, int64_t now)
{
    if (!db || !contact_id || !topic)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO active_threads (contact_id, topic, last_update_at, created_at) "
                      "VALUES (?, ?, ?, ?)";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_INTERNAL;
    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, topic, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_bind_int64(stmt, 4, now);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_INTERNAL;
}

hu_error_t hu_thread_resolve(sqlite3 *db, int64_t thread_id)
{
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE active_threads SET status='resolved' WHERE id=?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_INTERNAL;
    sqlite3_bind_int64(stmt, 1, thread_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_INTERNAL;
}

int hu_thread_list_open(sqlite3 *db, const char *contact_id,
    char topics[][128], int max_out)
{
    if (!db || !contact_id || !topics || max_out <= 0)
        return 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT topic FROM active_threads WHERE contact_id=? AND status='open' "
                      "ORDER BY last_update_at DESC LIMIT ?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, max_out);
    int count = 0;
    while (count < max_out && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(stmt, 0);
        if (t) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 0);
            if (len >= 128)
                len = 127;
            memcpy(topics[count], t, len);
            topics[count][len] = '\0';
        } else {
            topics[count][0] = '\0';
        }
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

int hu_thread_needs_followup(sqlite3 *db, const char *contact_id,
    int64_t min_age_sec, int64_t max_age_sec, int64_t now)
{
    if (!db || !contact_id)
        return 0;
    int64_t lo = now - max_age_sec;
    int64_t hi = now - min_age_sec;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT COUNT(*) FROM active_threads WHERE contact_id=? AND status='open' "
                      "AND last_update_at >= ? AND last_update_at <= ?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, lo);
    sqlite3_bind_int64(stmt, 3, hi);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

/* F115: Interaction quality */
hu_error_t hu_interaction_quality_record(sqlite3 *db, const char *contact_id,
    float quality_score, float cognitive_load, const char *mood_state, int64_t now)
{
    if (!db || !contact_id)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO interaction_quality (contact_id, quality_score, "
                      "cognitive_load, mood_state, timestamp) VALUES (?, ?, ?, ?, ?)";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_INTERNAL;
    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, (double)quality_score);
    sqlite3_bind_double(stmt, 3, (double)cognitive_load);
    if (mood_state)
        sqlite3_bind_text(stmt, 4, mood_state, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_int64(stmt, 5, now);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_INTERNAL;
}

int hu_interaction_quality_needs_recovery(sqlite3 *db, const char *contact_id,
    float threshold, int64_t min_age_sec, int64_t max_age_sec, int64_t now)
{
    if (!db || !contact_id)
        return 0;
    int64_t lo = now - max_age_sec;
    int64_t hi = now - min_age_sec;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT COUNT(*) FROM interaction_quality WHERE contact_id=? "
                      "AND recovery_sent=0 AND quality_score < ? "
                      "AND timestamp >= ? AND timestamp <= ?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, (double)threshold);
    sqlite3_bind_int64(stmt, 3, lo);
    sqlite3_bind_int64(stmt, 4, hi);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

hu_error_t hu_interaction_quality_mark_recovered(sqlite3 *db,
    const char *contact_id, int64_t now)
{
    if (!db || !contact_id)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE interaction_quality SET recovery_sent=1, recovery_at=? "
                      "WHERE contact_id=? AND recovery_sent=0";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_INTERNAL;
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, contact_id, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_INTERNAL;
}

#endif /* HU_ENABLE_SQLITE */
