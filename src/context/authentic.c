/* Phase 9 — F103–F115 Authentic Existence */
#include "human/context/authentic.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

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
