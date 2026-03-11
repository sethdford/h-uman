#include "human/persona/style_clone.h"
#include "human/core/string.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HU_STYLE_MSG_LIMIT 5000

static size_t count_substring(const char *haystack, const char *needle) {
    size_t count = 0;
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return 0;
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += nlen;
    }
    return count;
}

static size_t count_emoji_utf8(const char *s) {
    size_t count = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p >= 0xF0) {
            count++;
            if (p[1] && p[2] && p[3])
                p += 4;
            else
                break;
        } else if (*p >= 0xE0 && p[1] && p[2]) {
            p += 3;
        } else if (*p >= 0xC0 && p[1]) {
            p += 2;
        } else {
            p++;
        }
    }
    return count;
}

static size_t count_haha(const char *s) {
    size_t n = 0;
    const char *p = s;
    while ((p = strstr(p, "haha")) != NULL) {
        n++;
        p += 1; /* overlap: "hahaha" counts as 2 */
    }
    return n;
}

hu_error_t hu_style_build_query(const char *contact_id, size_t contact_id_len, char *buf,
                                 size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    if (!contact_id || contact_id_len == 0) {
        int n = snprintf(buf, cap,
                         "SELECT text FROM message "
                         "WHERE is_from_me = 1 AND text IS NOT NULL AND text != '' "
                         "AND length(text) > 0 ORDER BY date DESC LIMIT %u",
                         (unsigned)HU_STYLE_MSG_LIMIT);
        if (n < 0 || (size_t)n >= cap)
            return HU_ERR_INVALID_ARGUMENT;
        *out_len = (size_t)n;
        return HU_OK;
    }

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
                     "SELECT text FROM message "
                     "WHERE is_from_me = 1 AND text IS NOT NULL AND text != '' "
                     "AND length(text) > 0 "
                     "AND handle_id = (SELECT ROWID FROM handle WHERE id = '%s') "
                     "ORDER BY date DESC LIMIT %u",
                     sanitized, (unsigned)HU_STYLE_MSG_LIMIT);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_style_analyze_messages(hu_allocator_t *alloc, const char **messages,
                                     size_t msg_count, hu_style_fingerprint_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!messages || msg_count < 10)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    out->computed_at = (uint64_t)time(NULL);

    size_t processed = 0;
    size_t lowercase_count = 0;
    size_t sentence_case_count = 0;
    size_t period_ending_count = 0;
    size_t exclamation_ending_count = 0;
    size_t ellipsis_count = 0;
    size_t haha_count = 0;
    size_t lol_count = 0;
    size_t lmao_count = 0;
    size_t question_count = 0;
    size_t emoji_total = 0;
    double length_sum = 0.0;
    double length_sq_sum = 0.0;
    size_t double_text_count = 0;

    for (size_t i = 0; i < msg_count; i++) {
        const char *msg = messages[i];
        if (!msg)
            continue;
        size_t len = strlen(msg);

        const char *p = msg;
        while (*p && isspace((unsigned char)*p))
            p++;
        const char *start = p;
        while (*p)
            p++;
        while (p > start && isspace((unsigned char)*(p - 1)))
            p--;
        size_t content_len = (size_t)(p - msg);
        if (content_len == 0)
            continue;
        processed++;

        int has_upper = 0;
        int has_lower = 0;
        int first_upper = 0;
        for (const char *c = msg; *c; c++) {
            unsigned char u = (unsigned char)*c;
            if (u >= 0xF0) {
                c += 3;
                continue;
            }
            if (isupper(u))
                has_upper = 1;
            if (islower(u))
                has_lower = 1;
            if (c == msg && isupper(u))
                first_upper = 1;
        }
        if (!has_upper && has_lower)
            lowercase_count++;
        else if (first_upper && has_lower)
            sentence_case_count++;

        char last_char = '\0';
        for (const char *c = msg; *c; c++)
            if (!isspace((unsigned char)*c))
                last_char = *c;
        if (last_char == '.')
            period_ending_count++;
        else if (last_char == '!')
            exclamation_ending_count++;
        if (strstr(msg, "...") != NULL)
            ellipsis_count++;

        size_t h = count_haha(msg);
        size_t l = count_substring(msg, "lol");
        size_t lm = count_substring(msg, "lmao") + count_substring(msg, "lmfao");
        haha_count += h;
        lol_count += l;
        lmao_count += lm;

        length_sum += (double)len;
        length_sq_sum += (double)len * (double)len;

        emoji_total += count_emoji_utf8(msg);

        if (strchr(msg, '?') != NULL)
            question_count++;

        if (i + 1 < msg_count && messages[i + 1] != NULL)
            double_text_count++;
    }

    if (processed < 10)
        return HU_ERR_INVALID_ARGUMENT;

    out->sample_count = (uint32_t)processed;
    double n = (double)processed;
    out->lowercase_ratio = n > 0 ? (double)lowercase_count / n : 0.0;
    out->sentence_case_ratio = n > 0 ? (double)sentence_case_count / n : 0.0;
    out->period_ending_ratio = n > 0 ? (double)period_ending_count / n : 0.0;
    out->exclamation_ending_ratio = n > 0 ? (double)exclamation_ending_count / n : 0.0;
    out->ellipsis_ratio = n > 0 ? (double)ellipsis_count / n : 0.0;
    out->question_ratio = n > 0 ? (double)question_count / n : 0.0;
    out->emoji_per_message = n > 0 ? (double)emoji_total / n : 0.0;
    out->double_text_ratio = n > 0 ? (double)double_text_count / n : 0.0;

    size_t laugh_total = haha_count + lol_count + lmao_count;
    if (laugh_total > 0) {
        out->haha_ratio = (double)haha_count / (double)laugh_total;
        out->lol_ratio = (double)lol_count / (double)laugh_total;
        out->lmao_ratio = (double)lmao_count / (double)laugh_total;
    }

    out->avg_message_length = n > 0 ? length_sum / n : 0.0;
    if (n > 1) {
        double variance = (length_sq_sum - length_sum * length_sum / n) / (n - 1.0);
        out->msg_length_stddev = variance > 0.0 ? sqrt(variance) : 0.0;
    }

    return HU_OK;
}

hu_error_t hu_style_fingerprint_to_prompt(hu_allocator_t *alloc,
                                         const hu_style_fingerprint_t *fp, char **out,
                                         size_t *out_len) {
    if (!alloc || !fp || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    char buf[1024];
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "[STYLE for this contact]:\n");
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            "- Capitalization: mostly %s (%.0f%%)\n",
                            fp->lowercase_ratio > 0.5 ? "lowercase" : "sentence case",
                            fp->lowercase_ratio > 0.5 ? fp->lowercase_ratio * 100.0
                                                      : fp->sentence_case_ratio * 100.0);
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            "- Punctuation: %s periods (%.0f%%), %s ellipsis (%.0f%%)\n",
                            fp->period_ending_ratio < 0.3 ? "rarely uses" : "uses",
                            fp->period_ending_ratio * 100.0,
                            fp->ellipsis_ratio > 0.1 ? "occasional" : "rare",
                            fp->ellipsis_ratio * 100.0);
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            "- Laughter: \"haha\" (%.0f%%), \"lol\" (%.0f%%), \"lmao\" (%.0f%%)\n",
                            fp->haha_ratio * 100.0, fp->lol_ratio * 100.0, fp->lmao_ratio * 100.0);
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            "- Average message length: %.0f chars\n", fp->avg_message_length);
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            "- Emoji: %.1f per message\n", fp->emoji_per_message);
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            "- Sends follow-up messages %.0f%% of the time\n",
                            fp->double_text_ratio * 100.0);

    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}

void hu_style_fingerprint_deinit(hu_allocator_t *alloc, hu_style_fingerprint_t *fp) {
    if (!alloc || !fp)
        return;
    if (fp->contact_id) {
        size_t sz = fp->contact_id_len + 1;
        alloc->free(alloc->ctx, fp->contact_id, sz);
        fp->contact_id = NULL;
        fp->contact_id_len = 0;
    }
}
