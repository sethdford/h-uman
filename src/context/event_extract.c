#include "seaclaw/context/event_extract.h"
#include "seaclaw/core/string.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static char to_lower(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= 'A' && u <= 'Z') ? (char)(u + 32) : (char)u;
}

static bool prefix_match_ci(const char *text, size_t text_len, const char *prefix,
                            size_t prefix_len) {
    if (text_len < prefix_len)
        return false;
    for (size_t i = 0; i < prefix_len; i++) {
        if (to_lower(text[i]) != to_lower(prefix[i]))
            return false;
    }
    return true;
}

static bool is_word_char(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
           u == '\'' || u == '-';
}

static bool is_word_boundary(char c) {
    return c == '\0' || (unsigned char)c <= 32 || c == '.' || c == ',' || c == '!' || c == '?' ||
           c == ';' || c == ':';
}

typedef struct {
    const char *pattern;
    size_t pattern_len;
    double confidence;
} sc_temporal_pattern_t;

static const sc_temporal_pattern_t TEMPORAL_PATTERNS[] = {
    /* Relative - high specificity */
    {"tomorrow", 8, 0.85},
    {"today", 5, 0.85},
    {"yesterday", 9, 0.85},
    /* Relative - week/month */
    {"next week", 9, 0.75},
    {"this week", 9, 0.75},
    {"last week", 9, 0.75},
    {"next month", 10, 0.75},
    {"this month", 10, 0.75},
    {"last month", 10, 0.75},
    /* Day names with optional prefix - check longer first */
    {"on Monday", 9, 0.8},
    {"on Tuesday", 10, 0.8},
    {"on Wednesday", 12, 0.8},
    {"on Thursday", 11, 0.8},
    {"on Friday", 9, 0.8},
    {"on Saturday", 11, 0.8},
    {"on Sunday", 9, 0.8},
    {"this Monday", 11, 0.8},
    {"this Tuesday", 12, 0.8},
    {"this Wednesday", 14, 0.8},
    {"this Thursday", 13, 0.8},
    {"this Friday", 11, 0.8},
    {"this Saturday", 13, 0.8},
    {"this Sunday", 11, 0.8},
    {"next Monday", 11, 0.8},
    {"next Tuesday", 12, 0.8},
    {"next Wednesday", 14, 0.8},
    {"next Thursday", 13, 0.8},
    {"next Friday", 11, 0.8},
    {"next Saturday", 13, 0.8},
    {"next Sunday", 11, 0.8},
    {"Monday", 6, 0.75},
    {"Tuesday", 7, 0.75},
    {"Wednesday", 9, 0.75},
    {"Thursday", 8, 0.75},
    {"Friday", 6, 0.75},
    {"Saturday", 8, 0.75},
    {"Sunday", 6, 0.75},
    /* Month + date - exact date, highest confidence */
    {"January 1st", 11, 0.95},
    {"January 2nd", 11, 0.95},
    {"January 3rd", 11, 0.95},
    {"January 15th", 12, 0.95},
    {"January 5", 9, 0.95},
    {"February 15th", 14, 0.95},
    {"March 15th", 10, 0.95},
    {"March 15", 9, 0.95},
    {"April 15th", 11, 0.95},
    {"May 15th", 9, 0.95},
    {"June 15th", 10, 0.95},
    {"July 15th", 10, 0.95},
    {"August 15th", 12, 0.95},
    {"September 15th", 15, 0.95},
    {"October 15th", 13, 0.95},
    {"November 15th", 14, 0.95},
    {"December 15th", 14, 0.95},
    /* "on March 15th" etc */
    {"on January 15th", 16, 0.95},
    {"on February 15th", 17, 0.95},
    {"on March 15th", 13, 0.95},
    {"on April 15th", 14, 0.95},
    {"on May 15th", 12, 0.95},
    {"on June 15th", 13, 0.95},
    {"on July 15th", 13, 0.95},
    {"on August 15th", 15, 0.95},
    {"on September 15th", 18, 0.95},
    {"on October 15th", 16, 0.95},
    {"on November 15th", 17, 0.95},
    {"on December 15th", 17, 0.95},
    /* "on the 15th" */
    {"on the 1st", 10, 0.9},
    {"on the 2nd", 10, 0.9},
    {"on the 3rd", 10, 0.9},
    {"on the 15th", 11, 0.9},
    {"on the 21st", 11, 0.9},
    {"on the 22nd", 11, 0.9},
    {"on the 23rd", 11, 0.9},
    {"on the 31st", 11, 0.9},
    {"the 15th", 8, 0.85},
    /* "in X days" */
    {"in 1 day", 8, 0.8},
    {"in 2 days", 9, 0.8},
    {"in 7 days", 9, 0.8},
    {"in 14 days", 10, 0.8},
    {"in 1 week", 9, 0.8},
    {"in 2 weeks", 10, 0.8},
};

#define TEMPORAL_PATTERN_COUNT (sizeof(TEMPORAL_PATTERNS) / sizeof(TEMPORAL_PATTERNS[0]))

/* Match month name + optional space + day (1-31) + optional st/nd/rd/th */
static bool match_month_date(const char *text, size_t text_len, size_t *out_len, double *out_conf) {
    static const char *MONTHS[] = {"January", "February", "March", "April", "May", "June",
                                   "July", "August", "September", "October", "November",
                                   "December"};
    for (size_t m = 0; m < 12; m++) {
        size_t month_len = strlen(MONTHS[m]);
        if (text_len < month_len + 2)
            continue;
        if (!prefix_match_ci(text, text_len, MONTHS[m], month_len))
            continue;
        size_t i = month_len;
        while (i < text_len && (unsigned char)text[i] == ' ')
            i++;
        if (i >= text_len || !isdigit((unsigned char)text[i]))
            continue;
        size_t day_start = i;
        while (i < text_len && isdigit((unsigned char)text[i]))
            i++;
        if (i <= day_start)
            continue;
        size_t day_len = i - day_start;
        if (day_len > 2)
            continue;
        int day = 0;
        for (size_t d = day_start; d < i; d++)
            day = day * 10 + (text[d] - '0');
        if (day < 1 || day > 31)
            continue;
        size_t suffix_start = i;
        if (i < text_len && (text[i] == 's' || text[i] == 'n' || text[i] == 'r' || text[i] == 't')) {
            if (day == 1 || day == 21 || day == 31) {
                if (text_len - i >= 2 && to_lower(text[i]) == 's' && to_lower(text[i + 1]) == 't')
                    i += 2;
            } else if (day == 2 || day == 22) {
                if (text_len - i >= 2 && to_lower(text[i]) == 'n' && to_lower(text[i + 1]) == 'd')
                    i += 2;
            } else if (day == 3 || day == 23) {
                if (text_len - i >= 2 && to_lower(text[i]) == 'r' && to_lower(text[i + 1]) == 'd')
                    i += 2;
            } else {
                if (text_len - i >= 2 && to_lower(text[i]) == 't' && to_lower(text[i + 1]) == 'h')
                    i += 2;
            }
        }
        if (i > suffix_start && i < text_len && is_word_char((unsigned char)text[i]))
            continue;
        *out_len = i;
        *out_conf = 0.95;
        return true;
    }
    return false;
}

/* Match "on " + month date or "on the N(th)" */
static bool match_on_month_date(const char *text, size_t text_len, size_t *out_len,
                                double *out_conf) {
    if (text_len < 5)
        return false;
    if (!prefix_match_ci(text, 3, "on ", 3))
        return false;
    if (prefix_match_ci(text + 3, text_len - 3, "the ", 4)) {
        size_t i = 7;
        while (i < text_len && (unsigned char)text[i] == ' ')
            i++;
        if (i >= text_len || !isdigit((unsigned char)text[i]))
            return false;
        size_t num_start = i;
        while (i < text_len && isdigit((unsigned char)text[i]))
            i++;
        if (i <= num_start)
            return false;
        int day = 0;
        for (size_t d = num_start; d < i; d++)
            day = day * 10 + (text[d] - '0');
        if (day < 1 || day > 31)
            return false;
        if (i < text_len) {
            char c = to_lower(text[i]);
            if (c == 's' && i + 1 < text_len && to_lower(text[i + 1]) == 't')
                i += 2;
            else if (c == 'n' && i + 1 < text_len && to_lower(text[i + 1]) == 'd')
                i += 2;
            else if (c == 'r' && i + 1 < text_len && to_lower(text[i + 1]) == 'd')
                i += 2;
            else if (c == 't' && i + 1 < text_len && to_lower(text[i + 1]) == 'h')
                i += 2;
        }
        *out_len = i;
        *out_conf = 0.9;
        return true;
    }
    size_t inner_len;
    if (match_month_date(text + 3, text_len - 3, &inner_len, out_conf)) {
        *out_len = 3 + inner_len;
        return true;
    }
    return false;
}

/* Match "in N days" or "in N weeks" */
static bool match_in_days(const char *text, size_t text_len, size_t *out_len, double *out_conf) {
    if (text_len < 8)
        return false;
    if (!prefix_match_ci(text, 3, "in ", 3))
        return false;
    size_t i = 3;
    while (i < text_len && (unsigned char)text[i] == ' ')
        i++;
    if (i >= text_len || !isdigit((unsigned char)text[i]))
        return false;
    while (i < text_len && isdigit((unsigned char)text[i]))
        i++;
    while (i < text_len && (unsigned char)text[i] == ' ')
        i++;
    if (i >= text_len)
        return false;
    if (prefix_match_ci(text + i, text_len - i, "day", 3)) {
        if (i + 3 < text_len && is_word_char((unsigned char)text[i + 3]))
            return false;
        *out_len = i + 3;
        *out_conf = 0.8;
        return true;
    }
    if (prefix_match_ci(text + i, text_len - i, "days", 4)) {
        if (i + 4 < text_len && is_word_char((unsigned char)text[i + 4]))
            return false;
        *out_len = i + 4;
        *out_conf = 0.8;
        return true;
    }
    if (prefix_match_ci(text + i, text_len - i, "week", 4)) {
        if (i + 4 < text_len && is_word_char((unsigned char)text[i + 4]))
            return false;
        *out_len = i + 4;
        *out_conf = 0.8;
        return true;
    }
    if (prefix_match_ci(text + i, text_len - i, "weeks", 5)) {
        if (i + 5 < text_len && is_word_char((unsigned char)text[i + 5]))
            return false;
        *out_len = i + 5;
        *out_conf = 0.8;
        return true;
    }
    return false;
}

/* Find event description preceding temporal at temporal_start. Returns (desc_start, desc_len). */
static void extract_event_before(const char *text, size_t text_len, size_t temporal_start,
                                 size_t *desc_start, size_t *desc_len) {
    *desc_start = 0;
    *desc_len = 0;
    if (temporal_start == 0)
        return;

    size_t search_start = temporal_start;
    while (search_start > 0 && (unsigned char)text[search_start - 1] <= 32)
        search_start--;
    if (search_start == 0)
        return;

    /* "my X is " or "my X is on " - use temporal_start to include trailing space before temporal */
    if (search_start >= 4 && prefix_match_ci(text, 3, "my ", 3)) {
        const char *is_pos = NULL;
        for (size_t i = 3; i < temporal_start; i++) {
            size_t avail = temporal_start - i;
            if (avail >= 4 && prefix_match_ci(text + i, avail, " is ", 4)) {
                is_pos = text + i;
                break;
            }
            if (avail >= 7 && prefix_match_ci(text + i, avail, " is on ", 7)) {
                is_pos = text + i;
                break;
            }
        }
        if (is_pos) {
            *desc_start = 3;
            *desc_len = (size_t)(is_pos - text) - 3;
            while (*desc_len > 0 && (unsigned char)text[*desc_start + *desc_len - 1] <= 32)
                (*desc_len)--;
            while (*desc_len > 0 && (unsigned char)text[*desc_start] <= 32) {
                (*desc_start)++;
                (*desc_len)--;
            }
            return;
        }
    }

    /* "have a X " or "have an X " - search anywhere in preceding text */
    for (size_t i = 0; i + 8 <= search_start && i < text_len; i++) {
        size_t avail = (text_len - i < search_start - i) ? text_len - i : search_start - i;
        if (avail >= 7 && prefix_match_ci(text + i, avail, "have a ", 7) &&
            (i == 0 || is_word_boundary(text[i - 1]))) {
            *desc_start = i + 7;
            *desc_len = search_start - (i + 7);
            while (*desc_len > 0 && (unsigned char)text[*desc_start + *desc_len - 1] <= 32)
                (*desc_len)--;
            if (*desc_len > 0)
                return;
        }
    }
    for (size_t i = 0; i + 9 <= search_start && i < text_len; i++) {
        size_t avail = (text_len - i < search_start - i) ? text_len - i : search_start - i;
        if (avail >= 8 && prefix_match_ci(text + i, avail, "have an ", 8) &&
            (i == 0 || is_word_boundary(text[i - 1]))) {
            *desc_start = i + 8;
            *desc_len = search_start - (i + 8);
            while (*desc_len > 0 && (unsigned char)text[*desc_start + *desc_len - 1] <= 32)
                (*desc_len)--;
            if (*desc_len > 0)
                return;
        }
    }

    /* "X on " - event before " on " */
    for (size_t i = 1; i < search_start; i++) {
        if (search_start - i >= 4 && prefix_match_ci(text + i, (size_t)(search_start - i), " on ", 4)) {
            *desc_start = 0;
            *desc_len = i;
            while (*desc_len > 0 && (unsigned char)text[*desc_start + *desc_len - 1] <= 32)
                (*desc_len)--;
            if (*desc_len > 0)
                return;
        }
    }

    /* "X is " or "X is on " - event before connector */
    for (size_t i = 1; i < search_start; i++) {
        if (search_start - i >= 4 && prefix_match_ci(text + i, (size_t)(search_start - i), " is ", 4)) {
            *desc_start = 0;
            *desc_len = i;
            while (*desc_len > 0 && (unsigned char)text[*desc_start + *desc_len - 1] <= 32)
                (*desc_len)--;
            if (*desc_len > 0)
                return;
        }
        if (search_start - i >= 7 &&
            prefix_match_ci(text + i, (size_t)(search_start - i), " is on ", 7)) {
            *desc_start = 0;
            *desc_len = i;
            while (*desc_len > 0 && (unsigned char)text[*desc_start + *desc_len - 1] <= 32)
                (*desc_len)--;
            if (*desc_len > 0)
                return;
        }
    }

    /* Default: full preceding phrase before temporal */
    size_t end = search_start;
    while (end > 0 && (unsigned char)text[end - 1] <= 32)
        end--;
    size_t start = 0;
    while (start < end && (unsigned char)text[start] <= 32)
        start++;
    if (start < end) {
        *desc_start = start;
        *desc_len = end - start;
    }
}

static void add_event(sc_allocator_t *alloc, sc_event_extract_result_t *out, const char *desc,
                      size_t desc_len, const char *temporal, size_t temporal_len,
                      double confidence) {
    if (out->event_count >= SC_EVENT_MAX_EVENTS)
        return;
    if (desc_len == 0 && temporal_len == 0)
        return;

    /* Strip leading "on " from temporal_ref for canonical form */
    if (temporal_len >= 3 && prefix_match_ci(temporal, temporal_len, "on ", 3)) {
        temporal += 3;
        temporal_len -= 3;
        while (temporal_len > 0 && (unsigned char)*temporal == ' ') {
            temporal++;
            temporal_len--;
        }
    }

    sc_extracted_event_t *ev = &out->events[out->event_count];
    ev->description = (desc_len > 0) ? sc_strndup(alloc, desc, desc_len) : NULL;
    ev->description_len = ev->description ? desc_len : 0;
    ev->temporal_ref = (temporal_len > 0) ? sc_strndup(alloc, temporal, temporal_len) : NULL;
    ev->temporal_ref_len = ev->temporal_ref ? temporal_len : 0;
    ev->confidence = confidence;

    if ((desc_len == 0 || ev->description) && (temporal_len == 0 || ev->temporal_ref))
        out->event_count++;
    else {
        if (ev->description)
            alloc->free(alloc->ctx, ev->description, ev->description_len + 1);
        if (ev->temporal_ref)
            alloc->free(alloc->ctx, ev->temporal_ref, ev->temporal_ref_len + 1);
    }
}

sc_error_t sc_event_extract(sc_allocator_t *alloc, const char *text, size_t text_len,
                            sc_event_extract_result_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!text) {
        if (text_len > 0)
            return SC_ERR_INVALID_ARGUMENT;
        return SC_OK;
    }

    if (text_len == 0)
        return SC_OK;

    for (size_t i = 0; i < text_len && out->event_count < SC_EVENT_MAX_EVENTS; i++) {
        while (i < text_len && (unsigned char)text[i] <= 32)
            i++;
        if (i >= text_len)
            break;

        size_t temporal_len = 0;
        double confidence = 0.7;
        bool found = false;

        /* Try "on " prefix first for dates */
        if (text_len - i >= 5) {
            if (match_on_month_date(text + i, text_len - i, &temporal_len, &confidence)) {
                found = true;
            } else if (match_in_days(text + i, text_len - i, &temporal_len, &confidence)) {
                found = true;
            }
        }

        if (!found && text_len - i >= 4 && match_in_days(text + i, text_len - i, &temporal_len,
                                                         &confidence)) {
            found = true;
        }

        if (!found && text_len - i >= 6 && match_month_date(text + i, text_len - i, &temporal_len,
                                                            &confidence)) {
            found = true;
        }

        if (!found) {
            for (size_t p = 0; p < TEMPORAL_PATTERN_COUNT; p++) {
                const sc_temporal_pattern_t *pat = &TEMPORAL_PATTERNS[p];
                if (text_len - i >= pat->pattern_len &&
                    prefix_match_ci(text + i, text_len - i, pat->pattern, pat->pattern_len)) {
                    if (pat->pattern_len < text_len - i &&
                        is_word_char((unsigned char)text[i + pat->pattern_len]))
                        continue;
                    if (i > 0 && is_word_char((unsigned char)text[i - 1]))
                        continue;
                    temporal_len = pat->pattern_len;
                    confidence = pat->confidence;
                    found = true;
                    break;
                }
            }
        }

        if (!found)
            continue;

        size_t temporal_start = i;
        size_t desc_start, desc_len;
        extract_event_before(text, text_len, temporal_start, &desc_start, &desc_len);

        /* Require at least temporal; description can be empty for "tomorrow" etc */
        add_event(alloc, out, text + desc_start, desc_len, text + temporal_start, temporal_len,
                 confidence);

        i = temporal_start + temporal_len - 1;
    }

    return SC_OK;
}

void sc_event_extract_result_deinit(sc_event_extract_result_t *result, sc_allocator_t *alloc) {
    if (!result || !alloc)
        return;
    for (size_t i = 0; i < result->event_count; i++) {
        if (result->events[i].description)
            alloc->free(alloc->ctx, result->events[i].description,
                        result->events[i].description_len + 1);
        if (result->events[i].temporal_ref)
            alloc->free(alloc->ctx, result->events[i].temporal_ref,
                        result->events[i].temporal_ref_len + 1);
    }
    result->event_count = 0;
}
