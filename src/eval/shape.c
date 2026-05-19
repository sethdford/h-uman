/* src/eval/shape.c
 *
 * 2026-05-18 (M4): C-side deterministic shape classifier for eval responses.
 *
 * Mirrors scripts/eval_shape_classifier.py — same rules, same channel modes,
 * same per-fail bit flags. Called from hu_eval_run_suite to populate
 * hu_eval_result_t::shape_score automatically, so every eval persisted to
 * SQLite has a deterministic shape gate alongside the LLM-judge score.
 *
 * The LLM-judge has false positives (gives PASS to AI-assistant markdown)
 * AND false negatives (gives FAIL to peak-Seth single-sentence texts).
 * This classifier is the operational form of seth.json::anti_patterns —
 * regex/substring gates that detect canonical AI-assistant tells.
 */

#include "human/eval/shape.h"
#include <ctype.h>
#include <string.h>

/* Per-channel thresholds. Mirrors CHANNEL_RULES in
 * scripts/eval_shape_classifier.py — keep in sync. */
typedef struct shape_channel_rules {
    size_t too_long;
    size_t way_too_long;
    bool markdown_allowed;
    bool ai_openers_allowed;
} shape_channel_rules_t;

static const shape_channel_rules_t g_rules[] = {
    [HU_SHAPE_CHANNEL_IMESSAGE] = {.too_long = 250,
                                   .way_too_long = 500,
                                   .markdown_allowed = false,
                                   .ai_openers_allowed = false},
    [HU_SHAPE_CHANNEL_TELEGRAM] = {.too_long = 350,
                                   .way_too_long = 700,
                                   .markdown_allowed = false,
                                   .ai_openers_allowed = false},
    [HU_SHAPE_CHANNEL_DISCORD] = {.too_long = 500,
                                  .way_too_long = 1200,
                                  .markdown_allowed = true,
                                  .ai_openers_allowed = false},
    [HU_SHAPE_CHANNEL_SLACK] = {.too_long = 800,
                                .way_too_long = 2000,
                                .markdown_allowed = true,
                                .ai_openers_allowed = false},
    [HU_SHAPE_CHANNEL_EMAIL] = {.too_long = 2000,
                                .way_too_long = 5000,
                                .markdown_allowed = true,
                                .ai_openers_allowed = true},
};
static const size_t g_rules_count = sizeof(g_rules) / sizeof(g_rules[0]);

hu_shape_channel_t hu_shape_channel_from_string(const char *channel, size_t channel_len) {
    if (!channel || channel_len == 0)
        return HU_SHAPE_CHANNEL_IMESSAGE;
    if (channel_len == 8 && strncasecmp(channel, "imessage", 8) == 0)
        return HU_SHAPE_CHANNEL_IMESSAGE;
    if (channel_len == 8 && strncasecmp(channel, "telegram", 8) == 0)
        return HU_SHAPE_CHANNEL_TELEGRAM;
    if (channel_len == 7 && strncasecmp(channel, "discord", 7) == 0)
        return HU_SHAPE_CHANNEL_DISCORD;
    if (channel_len == 5 && strncasecmp(channel, "slack", 5) == 0)
        return HU_SHAPE_CHANNEL_SLACK;
    if (channel_len == 5 && strncasecmp(channel, "email", 5) == 0)
        return HU_SHAPE_CHANNEL_EMAIL;
    return HU_SHAPE_CHANNEL_IMESSAGE; /* default to strictest */
}

/* Case-insensitive prefix match, skipping leading whitespace. */
static bool starts_with_ci(const char *s, size_t s_len, const char *prefix) {
    size_t i = 0;
    while (i < s_len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
    size_t plen = strlen(prefix);
    if (s_len - i < plen)
        return false;
    for (size_t j = 0; j < plen; j++) {
        char a = s[i + j];
        char b = prefix[j];
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (b >= 'A' && b <= 'Z')
            b += 32;
        if (a != b)
            return false;
    }
    return true;
}

/* Detect a bullet list (lines starting with '*' or '-' followed by space).
 * Walks line starts; flags if any line begins with bullet marker. */
static bool contains_bullet_list(const char *s, size_t s_len) {
    if (!s || s_len == 0)
        return false;
    /* Check first line */
    size_t i = 0;
    while (i < s_len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i < s_len - 1 && (s[i] == '*' || s[i] == '-') && (s[i + 1] == ' ' || s[i + 1] == '\t'))
        return true;
    /* Check subsequent lines */
    for (size_t j = 0; j + 2 < s_len; j++) {
        if (s[j] != '\n')
            continue;
        size_t k = j + 1;
        while (k < s_len && (s[k] == ' ' || s[k] == '\t'))
            k++;
        if (k + 1 < s_len && (s[k] == '*' || s[k] == '-') && (s[k + 1] == ' ' || s[k + 1] == '\t'))
            return true;
    }
    return false;
}

static bool contains_numbered_list(const char *s, size_t s_len) {
    if (!s || s_len == 0)
        return false;
    /* Check first line */
    size_t i = 0;
    while (i < s_len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i + 2 < s_len && s[i] >= '0' && s[i] <= '9' && s[i + 1] == '.' && s[i + 2] == ' ')
        return true;
    /* Check subsequent lines */
    for (size_t j = 0; j + 3 < s_len; j++) {
        if (s[j] != '\n')
            continue;
        size_t k = j + 1;
        while (k < s_len && (s[k] == ' ' || s[k] == '\t'))
            k++;
        if (k + 2 < s_len && s[k] >= '0' && s[k] <= '9' && s[k + 1] == '.' && s[k + 2] == ' ')
            return true;
    }
    return false;
}

static bool contains_header(const char *s, size_t s_len) {
    if (!s || s_len == 0)
        return false;
    /* Check first line */
    if (s_len > 1 && s[0] == '#' && (s[1] == ' ' || s[1] == '#'))
        return true;
    /* Subsequent */
    for (size_t j = 0; j + 1 < s_len; j++) {
        if (s[j] != '\n')
            continue;
        if (s[j + 1] == '#' && j + 2 < s_len && (s[j + 2] == ' ' || s[j + 2] == '#'))
            return true;
    }
    return false;
}

static bool contains_bold_markdown(const char *s, size_t s_len) {
    /* Match **xxxxx** with at least 2 chars between */
    if (!s || s_len < 6)
        return false;
    for (size_t i = 0; i + 5 < s_len; i++) {
        if (s[i] != '*' || s[i + 1] != '*')
            continue;
        /* find closing ** */
        for (size_t j = i + 4; j + 1 < s_len; j++) {
            if (s[j] == '*' && s[j + 1] == '*')
                return true;
            if (s[j] == '\n')
                break;
        }
    }
    return false;
}

static bool contains_code_fence(const char *s, size_t s_len) {
    if (!s || s_len < 3)
        return false;
    for (size_t i = 0; i + 2 < s_len; i++) {
        if (s[i] == '`' && s[i + 1] == '`' && s[i + 2] == '`')
            return true;
    }
    return false;
}

hu_error_t hu_shape_classify(const char *response, size_t response_len, hu_shape_channel_t channel,
                             hu_shape_result_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->score = 0.0;
    out->passed = false;
    out->response_len = 0;
    out->fail_flags = 0;

    if ((size_t)channel >= g_rules_count)
        channel = HU_SHAPE_CHANNEL_IMESSAGE;
    const shape_channel_rules_t *rules = &g_rules[channel];

    if (!response) {
        out->fail_flags |= HU_SHAPE_FAIL_NULL_RESPONSE;
        return HU_OK;
    }

    /* Strip leading/trailing whitespace for length + content checks */
    size_t start = 0;
    while (start < response_len && (response[start] == ' ' || response[start] == '\t' ||
                                    response[start] == '\n' || response[start] == '\r'))
        start++;
    size_t end = response_len;
    while (end > start && (response[end - 1] == ' ' || response[end - 1] == '\t' ||
                           response[end - 1] == '\n' || response[end - 1] == '\r'))
        end--;
    size_t trimmed_len = end - start;
    out->response_len = trimmed_len;

    if (trimmed_len == 0) {
        out->fail_flags |= HU_SHAPE_FAIL_EMPTY_RESPONSE;
        return HU_OK;
    }

    const char *r = response + start;

    /* Length checks (channel-specific) */
    bool way_too_long = false;
    if (trimmed_len > rules->way_too_long) {
        out->fail_flags |= HU_SHAPE_FAIL_WAY_TOO_LONG;
        way_too_long = true;
    } else if (trimmed_len > rules->too_long) {
        out->fail_flags |= HU_SHAPE_FAIL_TOO_LONG;
    }

    /* AI-assistant tells (skip if channel allows them, e.g. email) */
    bool fatal_opener = false;
    if (!rules->ai_openers_allowed) {
        if (starts_with_ci(r, trimmed_len, "Depending on")) {
            out->fail_flags |= HU_SHAPE_FAIL_DEPENDING_ON;
            fatal_opener = true;
        }
        if (starts_with_ci(r, trimmed_len, "Here are a few") ||
            starts_with_ci(r, trimmed_len, "Here are some") ||
            starts_with_ci(r, trimmed_len, "Here are several") ||
            starts_with_ci(r, trimmed_len, "Here are the")) {
            out->fail_flags |= HU_SHAPE_FAIL_HERE_ARE;
            fatal_opener = true;
        }
        if (starts_with_ci(r, trimmed_len, "Certainly,") ||
            starts_with_ci(r, trimmed_len, "Certainly.") ||
            starts_with_ci(r, trimmed_len, "Certainly!")) {
            out->fail_flags |= HU_SHAPE_FAIL_CERTAINLY;
        }
        if (starts_with_ci(r, trimmed_len, "Absolutely,") ||
            starts_with_ci(r, trimmed_len, "Absolutely.") ||
            starts_with_ci(r, trimmed_len, "Absolutely!")) {
            out->fail_flags |= HU_SHAPE_FAIL_ABSOLUTELY;
        }
        /* Anywhere-in-response tells */
        for (size_t i = 0; i + 14 < trimmed_len; i++) {
            if (strncasecmp(r + i, "great question", 14) == 0) {
                out->fail_flags |= HU_SHAPE_FAIL_GREAT_QUESTION;
                break;
            }
        }
    }

    /* Markdown tells (skip if channel allows markdown) */
    bool fatal_md = false;
    if (!rules->markdown_allowed) {
        if (contains_bullet_list(r, trimmed_len)) {
            out->fail_flags |= HU_SHAPE_FAIL_BULLET_LIST;
            fatal_md = true;
        }
        if (contains_numbered_list(r, trimmed_len)) {
            out->fail_flags |= HU_SHAPE_FAIL_NUMBERED_LIST;
            fatal_md = true;
        }
        if (contains_header(r, trimmed_len)) {
            out->fail_flags |= HU_SHAPE_FAIL_HEADER;
        }
        if (contains_bold_markdown(r, trimmed_len)) {
            out->fail_flags |= HU_SHAPE_FAIL_BOLD_MARKDOWN;
        }
        if (contains_code_fence(r, trimmed_len)) {
            out->fail_flags |= HU_SHAPE_FAIL_CODE_FENCE;
        }
    }

    /* Score: start at 1.0, subtract per-fail penalty, clamp to [0, 1]
     * Mirrors the Python classifier — heavy violations -0.3, light -0.15. */
    double score = 1.0;
    uint32_t f = out->fail_flags;
    if (f & HU_SHAPE_FAIL_WAY_TOO_LONG)
        score -= 0.3;
    else if (f & HU_SHAPE_FAIL_TOO_LONG)
        score -= 0.15;
    if (f & HU_SHAPE_FAIL_BULLET_LIST)
        score -= 0.3;
    if (f & HU_SHAPE_FAIL_NUMBERED_LIST)
        score -= 0.3;
    if (f & HU_SHAPE_FAIL_HEADER)
        score -= 0.3;
    if (f & HU_SHAPE_FAIL_CODE_FENCE)
        score -= 0.3;
    if (f & HU_SHAPE_FAIL_BOLD_MARKDOWN)
        score -= 0.15;
    if (f & HU_SHAPE_FAIL_DEPENDING_ON)
        score -= 0.15;
    if (f & HU_SHAPE_FAIL_HERE_ARE)
        score -= 0.15;
    if (f & HU_SHAPE_FAIL_CERTAINLY)
        score -= 0.15;
    if (f & HU_SHAPE_FAIL_ABSOLUTELY)
        score -= 0.15;
    if (f & HU_SHAPE_FAIL_GREAT_QUESTION)
        score -= 0.15;
    if (f & HU_SHAPE_FAIL_I_UNDERSTAND)
        score -= 0.15;
    if (score < 0.0)
        score = 0.0;
    if (score > 1.0)
        score = 1.0;
    out->score = score;
    out->passed = (score >= 0.7) && !fatal_md && !way_too_long;

    /* Suppress unused variable warning if fatal_opener isn't used in the
     * pass logic. We track it for future weighting changes. */
    (void)fatal_opener;
    return HU_OK;
}
