/* Replay auto-tune: aggregate replay insights from LTM into cumulative feedback. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/persona/auto_tune.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_AUTO_TUNE_MAX_SIGNALS 64
#define HU_AUTO_TUNE_MAX_SIGNAL_LEN 128

typedef struct {
    char *key;
    int count;
} signal_count_t;

static void trim_and_lower(char *buf, size_t *len) {
    if (!buf || *len == 0)
        return;
    /* Trim leading */
    size_t i = 0;
    while (i < *len && (unsigned char)buf[i] <= ' ')
        i++;
    size_t j = *len;
    while (j > i && (unsigned char)buf[j - 1] <= ' ')
        j--;
    if (i > 0 || j < *len) {
        memmove(buf, buf + i, j - i);
        buf[j - i] = '\0';
        *len = j - i;
    }
    for (size_t k = 0; k < *len && buf[k]; k++)
        buf[k] = (char)(unsigned char)tolower((unsigned char)buf[k]);
}

static int find_or_add_signal(signal_count_t *signals, size_t *n, int positive,
                              const char *text, size_t text_len, hu_allocator_t *alloc) {
    (void)positive;
    if (!text || text_len == 0 || *n >= HU_AUTO_TUNE_MAX_SIGNALS)
        return -1;

    char norm[HU_AUTO_TUNE_MAX_SIGNAL_LEN];
    size_t copy_len = text_len < sizeof(norm) - 1 ? text_len : sizeof(norm) - 1;
    memcpy(norm, text, copy_len);
    norm[copy_len] = '\0';
    trim_and_lower(norm, &copy_len);
    if (copy_len == 0)
        return -1;

    for (size_t i = 0; i < *n; i++) {
        if (signals[i].key && strcmp(signals[i].key, norm) == 0) {
            signals[i].count++;
            return 0;
        }
    }
    signals[*n].key = hu_strndup(alloc, norm, copy_len);
    if (!signals[*n].key)
        return -1;
    signals[*n].count = 1;
    (*n)++;
    return 0;
}

static void free_signals(signal_count_t *signals, size_t n, hu_allocator_t *alloc) {
    for (size_t i = 0; i < n; i++) {
        if (signals[i].key) {
            alloc->free(alloc->ctx, signals[i].key, strlen(signals[i].key) + 1);
            signals[i].key = NULL;
        }
    }
}

/* Parse insight content and extract positive/negative signals. */
static void parse_insight_content(const char *content, size_t content_len,
                                  signal_count_t *pos_signals, size_t *pos_n,
                                  signal_count_t *neg_signals, size_t *neg_n,
                                  hu_allocator_t *alloc) {
    if (!content || content_len == 0)
        return;

    const char *p = content;
    const char *end = content + content_len;
    bool in_worked = false;
    bool in_improve = false;

    while (p < end) {
        const char *line_end = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!line_end)
            line_end = end;

        size_t line_len = (size_t)(line_end - p);
        if (line_len > 0) {
            /* Skip leading whitespace */
            while (line_len > 0 && (unsigned char)*p <= ' ')
                p++, line_len--;

            if (line_len >= 9 && memcmp(p, "POSITIVE:", 9) == 0) {
                const char *sig = p + 9;
                size_t sig_len = line_len - 9;
                while (sig_len > 0 && (unsigned char)*sig <= ' ')
                    sig++, sig_len--;
                if (sig_len > 0)
                    find_or_add_signal(pos_signals, pos_n, 1, sig, sig_len, alloc);
            } else if (line_len >= 9 && memcmp(p, "NEGATIVE:", 9) == 0) {
                const char *sig = p + 9;
                size_t sig_len = line_len - 9;
                while (sig_len > 0 && (unsigned char)*sig <= ' ')
                    sig++, sig_len--;
                if (sig_len > 0)
                    find_or_add_signal(neg_signals, neg_n, 0, sig, sig_len, alloc);
            } else if (line_len >= 12 && memcmp(p, "What worked:", 12) == 0) {
                in_worked = true;
                in_improve = false;
            } else if (line_len >= 17 && memcmp(p, "What to improve:", 17) == 0) {
                in_improve = true;
                in_worked = false;
            } else if ((in_worked || in_improve) && line_len >= 2 && p[0] == '-' && p[1] == ' ') {
                const char *sig = p + 2;
                size_t sig_len = line_len - 2;
                while (sig_len > 0 && (unsigned char)*sig <= ' ')
                    sig++, sig_len--;
                if (sig_len > 0) {
                    if (in_worked)
                        find_or_add_signal(pos_signals, pos_n, 1, sig, sig_len, alloc);
                    else
                        find_or_add_signal(neg_signals, neg_n, 0, sig, sig_len, alloc);
                }
            } else if (line_len > 0 && *p != '-' && *p != '#') {
                in_worked = false;
                in_improve = false;
            }
        }
        p = line_end + (line_end < end ? 1 : 0);
    }
}

/* Simple sort by count descending. */
static void sort_signals_by_count(signal_count_t *signals, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        size_t best = i;
        for (size_t j = i + 1; j < n; j++) {
            if (signals[j].count > (int)signals[best].count)
                best = j;
        }
        if (best != i) {
            signal_count_t tmp = signals[i];
            signals[i] = signals[best];
            signals[best] = tmp;
        }
    }
}

hu_error_t hu_replay_auto_tune(hu_allocator_t *alloc, hu_memory_t *memory,
                              const char *contact_id, size_t contact_id_len,
                              char **summary_out, size_t *summary_len) {
    if (!alloc || !summary_out || !summary_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!memory || !memory->vtable || !memory->vtable->list)
        return HU_ERR_INVALID_ARGUMENT;

    *summary_out = NULL;
    *summary_len = 0;

    static const char cat_name[] = "replay_insights";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = cat_name, .name_len = sizeof(cat_name) - 1},
    };

    hu_memory_entry_t *entries = NULL;
    size_t entry_count = 0;
    hu_error_t err = memory->vtable->list(memory->ctx, alloc, &cat, contact_id, contact_id_len,
                                          &entries, &entry_count);
    if (err != HU_OK)
        return err;

    signal_count_t pos_signals[HU_AUTO_TUNE_MAX_SIGNALS];
    signal_count_t neg_signals[HU_AUTO_TUNE_MAX_SIGNALS];
    memset(pos_signals, 0, sizeof(pos_signals));
    memset(neg_signals, 0, sizeof(neg_signals));
    size_t pos_n = 0;
    size_t neg_n = 0;

    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].content && entries[i].content_len > 0)
            parse_insight_content(entries[i].content, entries[i].content_len, pos_signals, &pos_n,
                                 neg_signals, &neg_n, alloc);
    }

    if (entries) {
        for (size_t i = 0; i < entry_count; i++)
            hu_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, entry_count * sizeof(hu_memory_entry_t));
    }

    int total_pos = 0;
    int total_neg = 0;
    for (size_t i = 0; i < pos_n; i++)
        total_pos += pos_signals[i].count;
    for (size_t i = 0; i < neg_n; i++)
        total_neg += neg_signals[i].count;

    if (pos_n == 0 && neg_n == 0) {
        free_signals(pos_signals, pos_n, alloc);
        free_signals(neg_signals, neg_n, alloc);
        return HU_OK;
    }

    sort_signals_by_count(pos_signals, pos_n);
    sort_signals_by_count(neg_signals, neg_n);

    size_t cap = 512;
    for (size_t i = 0; i < pos_n && i < 16; i++)
        cap += 64 + 32;
    for (size_t i = 0; i < neg_n && i < 16; i++)
        cap += 64 + 32;

    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        free_signals(pos_signals, pos_n, alloc);
        free_signals(neg_signals, neg_n, alloc);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t pos = 0;
    int w = snprintf(buf, cap, "### Conversation Feedback (accumulated)\nWHAT'S WORKING:\n");
    if (w > 0 && (size_t)w < cap)
        pos += (size_t)w;

    for (size_t i = 0; i < pos_n && i < 16 && pos < cap; i++) {
        if (pos_signals[i].key)
            w = snprintf(buf + pos, cap - pos, "- %s (observed %d times)\n", pos_signals[i].key,
                        pos_signals[i].count);
        if (w > 0 && pos + (size_t)w < cap)
            pos += (size_t)w;
    }

    if (pos < cap) {
        w = snprintf(buf + pos, cap - pos, "WHAT TO AVOID:\n");
        if (w > 0 && pos + (size_t)w < cap)
            pos += (size_t)w;
    }
    for (size_t i = 0; i < neg_n && i < 16 && pos < cap; i++) {
        if (neg_signals[i].key)
            w = snprintf(buf + pos, cap - pos, "- %s (observed %d times)\n", neg_signals[i].key,
                        neg_signals[i].count);
        if (w > 0 && pos + (size_t)w < cap)
            pos += (size_t)w;
    }

    int total = total_pos + total_neg;
    int pct_pos = total > 0 ? (total_pos * 100 / total) : 0;
    int pct_neg = total > 0 ? (total_neg * 100 / total) : 0;
    if (pos < cap) {
        w = snprintf(buf + pos, cap - pos,
                     "Overall: %d%% positive signals, %d%% negative signals.\n", pct_pos, pct_neg);
        if (w > 0 && pos + (size_t)w < cap)
            pos += (size_t)w;
    }

    free_signals(pos_signals, pos_n, alloc);
    free_signals(neg_signals, neg_n, alloc);

    *summary_out = hu_strndup(alloc, buf, pos);
    alloc->free(alloc->ctx, buf, cap);
    if (!*summary_out)
        return HU_ERR_OUT_OF_MEMORY;
    *summary_len = pos;
    return HU_OK;
}

char *hu_replay_tune_build_context(hu_allocator_t *alloc, const char *summary, size_t summary_len,
                                   size_t *out_len) {
    if (!alloc || !out_len)
        return NULL;
    *out_len = 0;
    if (!summary || summary_len == 0)
        return NULL;

    size_t cap = summary_len + 128;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return NULL;

    int w = snprintf(buf, cap, "\n### Tone Calibration\n%.*s\nApply these learned patterns to "
                               "this conversation.\n",
                    (int)summary_len, summary);
    if (w <= 0 || (size_t)w >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }

    char *out = hu_strndup(alloc, buf, (size_t)w);
    alloc->free(alloc->ctx, buf, cap);
    if (out)
        *out_len = (size_t)w;
    return out;
}
