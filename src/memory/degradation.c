#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory/degradation.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* --- LCG for deterministic randomness --- */
static double lcg_roll(uint32_t *state) {
    *state = (uint32_t)((uint64_t)*state * 1103515245U + 12345U);
    return ((*state >> 16) & 0x7FFFU) / 32767.0;
}

/* --- Keyword matching (case-insensitive) --- */
static bool strncasecmp_eq(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static bool contains_word(const char *content, size_t len, const char *word,
                          size_t word_len) {
    if (word_len == 0 || word_len > len)
        return false;
    for (size_t i = 0; i + word_len <= len; i++) {
        if (strncasecmp_eq(content + i, word_len, word, word_len)) {
            bool before_ok = (i == 0) || !isalnum((unsigned char)content[i - 1]);
            bool after_ok =
                (i + word_len >= len) || !isalnum((unsigned char)content[i + word_len]);
            if (before_ok && after_ok)
                return true;
        }
    }
    return false;
}

/* Commitment: promised, commitment, i'll, i will, deadline, swear */
static const struct {
    const char *word;
    size_t len;
} COMMITMENT[] = {
    {"promised", 8}, {"commitment", 10}, {"i'll", 4}, {"i will", 6},
    {"deadline", 8}, {"swear", 5},
};
static const size_t COMMITMENT_COUNT = sizeof(COMMITMENT) / sizeof(COMMITMENT[0]);

/* Emotional: love, died, funeral, hospital, cancer, divorce, pregnant */
static const struct {
    const char *word;
    size_t len;
} EMOTIONAL[] = {
    {"love", 4}, {"died", 4}, {"funeral", 7}, {"hospital", 8},
    {"cancer", 6}, {"divorce", 7}, {"pregnant", 8},
};
static const size_t EMOTIONAL_COUNT = sizeof(EMOTIONAL) / sizeof(EMOTIONAL[0]);

bool hu_degradation_is_protected(const char *content, size_t content_len) {
    if (!content || content_len == 0)
        return false;

    for (size_t i = 0; i < COMMITMENT_COUNT; i++) {
        if (contains_word(content, content_len, COMMITMENT[i].word, COMMITMENT[i].len))
            return true;
    }
    for (size_t i = 0; i < EMOTIONAL_COUNT; i++) {
        if (contains_word(content, content_len, EMOTIONAL[i].word, EMOTIONAL[i].len))
            return true;
    }

    static const char *skip_caps[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
        "Saturday", "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December",
        "The", "This", "That", "These", "Those", "There", "Then", "But",
        "And", "For", "Not", "Yet", "So", "Or", "We", "He", "She", "It",
        "A", "I", "In", "On", "At", "To", "My", "Our", "If", "No",
    };
    static const size_t skip_caps_count = sizeof(skip_caps) / sizeof(skip_caps[0]);

    bool at_sentence_start = true;
    for (size_t i = 0; i < content_len; i++) {
        if (isalpha((unsigned char)content[i]) && isupper((unsigned char)content[i])) {
            size_t word_start = i;
            while (i < content_len && isalpha((unsigned char)content[i]))
                i++;
            size_t word_len = i - word_start;
            if (!at_sentence_start) {
                bool is_common = false;
                for (size_t s = 0; s < skip_caps_count; s++) {
                    if (strlen(skip_caps[s]) == word_len &&
                        strncasecmp(content + word_start, skip_caps[s], word_len) == 0) {
                        is_common = true;
                        break;
                    }
                }
                if (!is_common)
                    return true;
            }
            if (word_start + word_len > 0 && content[word_start + word_len - 1] == '.')
                at_sentence_start = true;
            else
                at_sentence_start = false;
            if (i >= content_len)
                break;
            i--;
        } else if (content[i] == '.' && i + 1 < content_len) {
            at_sentence_start = true;
        } else if (!isspace((unsigned char)content[i]) && content[i] != '.') {
            at_sentence_start = false;
        }
    }
    return false;
}

hu_degradation_type_t hu_degradation_roll(const hu_degradation_config_t *config,
                                          uint32_t seed) {
    if (!config)
        return HU_DEGRADE_NONE;

    uint32_t state = seed;
    double roll = lcg_roll(&state);

    double perfect = config->perfect_rate;
    double fuzz = config->fuzz_rate;

    if (roll < perfect)
        return HU_DEGRADE_NONE;
    if (roll < perfect + fuzz)
        return HU_DEGRADE_FUZZ;
    return HU_DEGRADE_ASK_REMIND;
}

/* Day names for FUZZ replacement (shift forward) */
static const struct {
    const char *name;
    size_t len;
    const char *replacement;
    size_t repl_len;
} DAYS[] = {
    {"Sunday", 6, "Monday", 6},     {"Monday", 6, "Tuesday", 7},
    {"Tuesday", 7, "Wednesday", 9}, {"Wednesday", 9, "Thursday", 8},
    {"Thursday", 8, "Friday", 6},   {"Friday", 6, "Saturday", 8},
    {"Saturday", 8, "Sunday", 6},
};
static const size_t DAYS_COUNT = sizeof(DAYS) / sizeof(DAYS[0]);

static void apply_fuzz_digit(char *out, size_t *out_pos, char c) {
    if (c >= '0' && c <= '8') {
        out[(*out_pos)++] = (char)(c + 1);
    } else if (c == '9') {
        out[(*out_pos)++] = '8';
    } else {
        out[(*out_pos)++] = c;
    }
}

/* "last week" -> "a couple weeks ago" */
static const char LAST_WEEK[] = "last week";
static const size_t LAST_WEEK_LEN = 9;
static const char COUPLE_WEEKS[] = "a couple weeks ago";
static const size_t COUPLE_WEEKS_LEN = 18;

static char *apply_fuzz(hu_allocator_t *alloc, const char *content, size_t content_len,
                        uint32_t seed) {
    char *out = alloc->alloc(alloc->ctx, content_len + 256);
    if (!out)
        return NULL;

    size_t pos = 0;
    (void)seed;

    for (size_t i = 0; i < content_len; i++) {
        /* Check "last week" */
        if (i + LAST_WEEK_LEN <= content_len &&
            strncasecmp_eq(content + i, LAST_WEEK_LEN, LAST_WEEK, LAST_WEEK_LEN)) {
            bool boundary_before = (i == 0) || !isalnum((unsigned char)content[i - 1]);
            bool boundary_after =
                (i + LAST_WEEK_LEN >= content_len) ||
                !isalnum((unsigned char)content[i + LAST_WEEK_LEN]);
            if (boundary_before && boundary_after) {
                memcpy(out + pos, COUPLE_WEEKS, COUPLE_WEEKS_LEN);
                pos += COUPLE_WEEKS_LEN;
                i += LAST_WEEK_LEN - 1;
                continue;
            }
        }

        /* Check day names */
        bool day_matched = false;
        for (size_t d = 0; d < DAYS_COUNT && !day_matched; d++) {
            if (i + DAYS[d].len <= content_len &&
                strncasecmp_eq(content + i, DAYS[d].len, DAYS[d].name, DAYS[d].len)) {
                bool boundary_before = (i == 0) || !isalnum((unsigned char)content[i - 1]);
                bool boundary_after =
                    (i + DAYS[d].len >= content_len) ||
                    !isalnum((unsigned char)content[i + DAYS[d].len]);
                if (boundary_before && boundary_after) {
                    memcpy(out + pos, DAYS[d].replacement, DAYS[d].repl_len);
                    pos += DAYS[d].repl_len;
                    i += DAYS[d].len - 1;
                    day_matched = true;
                }
            }
        }
        if (day_matched)
            continue;

        /* Single digits */
        if (content[i] >= '0' && content[i] <= '9') {
            bool standalone = (i == 0 || !isdigit((unsigned char)content[i - 1])) &&
                             (i + 1 >= content_len || !isdigit((unsigned char)content[i + 1]));
            if (standalone) {
                apply_fuzz_digit(out, &pos, content[i]);
                continue;
            }
        }

        out[pos++] = content[i];
    }
    out[pos] = '\0';

    char *result = hu_strndup(alloc, out, pos);
    alloc->free(alloc->ctx, out, content_len + 256);
    return result;
}

static void first_n_words(const char *s, size_t len, size_t n, char *out, size_t out_cap,
                          size_t *out_len) {
    size_t pos = 0;
    size_t words = 0;
    bool in_word = false;
    size_t word_start = 0;

    for (size_t i = 0; i <= len && words < n; i++) {
        if (i < len && (isalnum((unsigned char)s[i]) || s[i] == '\'')) {
            if (!in_word) {
                word_start = i;
                in_word = true;
            }
        } else {
            if (in_word) {
                size_t wlen = i - word_start;
                if (pos + wlen + 2 < out_cap) {
                    if (pos > 0)
                        out[pos++] = ' ';
                    memcpy(out + pos, s + word_start, wlen);
                    pos += wlen;
                }
                words++;
                in_word = false;
            }
        }
    }
    out[pos] = '\0';
    *out_len = pos;
}

char *hu_degradation_apply(hu_allocator_t *alloc, const char *content,
                           size_t content_len, hu_degradation_type_t type,
                           uint32_t seed, size_t *out_len) {
    if (!alloc || !content || !out_len)
        return NULL;

    switch (type) {
    case HU_DEGRADE_NONE: {
        char *r = hu_strndup(alloc, content, content_len);
        if (r)
            *out_len = content_len;
        return r;
    }
    case HU_DEGRADE_FUZZ: {
        char *r = apply_fuzz(alloc, content, content_len, seed);
        if (r)
            *out_len = strlen(r);
        return r;
    }
    case HU_DEGRADE_ASK_REMIND: {
        char topic[128];
        size_t topic_len;
        first_n_words(content, content_len, 5, topic, sizeof(topic), &topic_len);
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "remind me, what was %s...?", topic);
        if (n < 0 || (size_t)n >= sizeof(buf))
            return NULL;
        char *r = hu_strndup(alloc, buf, (size_t)n);
        if (r)
            *out_len = (size_t)n;
        return r;
    }
    }
    return NULL;
}

/* --- F61 Memory Degradation API --- */

bool hu_memory_degradation_is_protected(const char *content, size_t len) {
    return hu_degradation_is_protected(content, len);
}

static void first_three_words(const char *s, size_t len, char *out, size_t out_cap,
                              size_t *out_len) {
    first_n_words(s, len, 3, out, out_cap, out_len);
}

char *hu_memory_degradation_apply(hu_allocator_t *alloc, const char *content, size_t content_len,
                                  uint32_t seed, float rate, size_t *out_len) {
    if (!alloc || !content || !out_len)
        return NULL;

    if (hu_memory_degradation_is_protected(content, content_len)) {
        char *r = hu_strndup(alloc, content, content_len);
        if (r)
            *out_len = content_len;
        return r;
    }

    /* Integer arithmetic to avoid floating-point boundary issues */
    int roll_pct = (int)(seed % 100);
    int unchanged_pct = (int)((1.0 - (double)rate) * 100);

    if (roll_pct < unchanged_pct) {
        char *r = hu_strndup(alloc, content, content_len);
        if (r)
            *out_len = content_len;
        return r;
    }

    if (roll_pct < 95) {
        char *r = apply_fuzz(alloc, content, content_len, seed);
        if (r)
            *out_len = strlen(r);
        return r;
    }

    /* 0.95 <= roll < 1.0: "remind me, what was that about [first 3 words]?" */
    {
        char topic[128];
        size_t topic_len;
        first_three_words(content, content_len, topic, sizeof(topic), &topic_len);
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "remind me, what was that about %s?",
                         topic_len > 0 ? topic : "...");
        if (n < 0 || (size_t)n >= sizeof(buf))
            return NULL;
        char *r = hu_strndup(alloc, buf, (size_t)n);
        if (r)
            *out_len = (size_t)n;
        return r;
    }
}

char *hu_degradation_process(hu_allocator_t *alloc, const char *content,
                             size_t content_len,
                             const hu_degradation_config_t *config, uint32_t seed,
                             size_t *out_len) {
    if (!alloc || !content || !out_len)
        return NULL;

    if (hu_degradation_is_protected(content, content_len)) {
        char *r = hu_strndup(alloc, content, content_len);
        if (r)
            *out_len = content_len;
        return r;
    }

    hu_degradation_config_t cfg = config ? *config
                                        : (hu_degradation_config_t){
                                              .perfect_rate = 0.90,
                                              .fuzz_rate = 0.05,
                                              .ask_rate = 0.05,
                                          };
    hu_degradation_type_t t = hu_degradation_roll(&cfg, seed);
    return hu_degradation_apply(alloc, content, content_len, t, seed, out_len);
}

/* --- Forgetting Curve --- */

double hu_forgetting_retention(double hours_since_recall, uint32_t rehearsal_count,
                               const hu_forgetting_config_t *config) {
    if (!config)
        return 1.0;

    uint32_t r = (rehearsal_count == 0) ? 1 : rehearsal_count;
    double effective_stability =
        config->stability_factor * pow((double)r, 1.0 / (double)config->rehearsal_boost);
    double retention =
        exp(-hours_since_recall / (effective_stability * 24.0));
    return retention > config->min_retention ? retention : config->min_retention;
}

bool hu_forgetting_should_recall(double hours_since_recall, uint32_t rehearsal_count,
                                const hu_forgetting_config_t *config, uint32_t seed) {
    double retention = hu_forgetting_retention(hours_since_recall, rehearsal_count, config);
    uint32_t state = seed;
    double roll = lcg_roll(&state);
    return roll < retention;
}

hu_error_t hu_forgetting_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS memory_recall_log (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    memory_id INTEGER NOT NULL,\n"
        "    recalled_at INTEGER NOT NULL\n"
        ")";

    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_forgetting_log_recall_sql(int64_t memory_id, uint64_t recalled_at,
                                        char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                     "INSERT INTO memory_recall_log (memory_id, recalled_at) VALUES "
                     "(%lld, %llu)",
                     (long long)memory_id, (unsigned long long)recalled_at);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_forgetting_query_recalls_sql(int64_t memory_id, char *buf, size_t cap,
                                           size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                     "SELECT id, memory_id, recalled_at FROM memory_recall_log "
                     "WHERE memory_id = %lld ORDER BY recalled_at DESC",
                     (long long)memory_id);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}
