#include "seaclaw/context/conversation.h"
#include "seaclaw/core/string.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CTX_BUF_CAP 16384

/* Safe pos advance: snprintf returns the would-be length even when truncated.
 * Clamp to remaining buffer capacity to prevent out-of-bounds writes. */
#define POS_ADVANCE(w, pos, cap)       \
    do {                               \
        if ((w) > 0) {                 \
            size_t _add = (size_t)(w); \
            if (_add > (cap) - (pos))  \
                _add = (cap) - (pos);  \
            (pos) += _add;             \
        }                              \
    } while (0)

static bool str_contains_ci(const char *haystack, size_t hlen, const char *needle)
    __attribute__((unused));
static bool str_contains_ci(const char *haystack, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen > hlen)
        return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

#define SC_CALLBACK_MAX_TOPICS 32
#define SC_CALLBACK_TOPIC_BUF  64
#define SC_CALLBACK_SCORE_MIN  3

typedef struct {
    char phrase[SC_CALLBACK_TOPIC_BUF];
    size_t phrase_len;
    size_t first_idx;
    size_t last_idx;
    int turn_count;
    bool has_unresolved_question;
    int score;
} sc_callback_topic_t;

static void extract_topics_from_text(const char *text, size_t text_len, sc_callback_topic_t *topics,
                                     size_t *topic_count) {
    if (!text || text_len == 0 || !topics || !topic_count || *topic_count >= SC_CALLBACK_MAX_TOPICS)
        return;
    const char *p = text;
    const char *end = text + text_len;
    while (p < end && *topic_count < SC_CALLBACK_MAX_TOPICS) {
        while (p < end && !isalnum((unsigned char)*p) && *p != '"' && *p != '\'')
            p++;
        if (p >= end)
            break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            const char *start = p;
            while (p < end && *p != q)
                p++;
            if (p > start && p - start < SC_CALLBACK_TOPIC_BUF - 1) {
                size_t len = (size_t)(p - start);
                memcpy(topics[*topic_count].phrase, start, len);
                topics[*topic_count].phrase[len] = '\0';
                topics[*topic_count].phrase_len = len;
                (*topic_count)++;
            }
            if (p < end)
                p++;
            continue;
        }
        if (p + 6 <= end && strncasecmp(p, "about ", 6) == 0) {
            const char *trigger = p + 6;
            p = trigger;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_' || *p == '-'))
                p++;
            if (p > trigger && (size_t)(p - trigger) < SC_CALLBACK_TOPIC_BUF - 1) {
                size_t len = (size_t)(p - trigger);
                memcpy(topics[*topic_count].phrase, trigger, len);
                topics[*topic_count].phrase[len] = '\0';
                topics[*topic_count].phrase_len = len;
                (*topic_count)++;
            }
            continue;
        }
        if (p + 10 <= end && strncasecmp(p, "regarding ", 10) == 0) {
            const char *trigger = p + 10;
            p = trigger;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_' || *p == '-'))
                p++;
            if (p > trigger && (size_t)(p - trigger) < SC_CALLBACK_TOPIC_BUF - 1) {
                size_t len = (size_t)(p - trigger);
                memcpy(topics[*topic_count].phrase, trigger, len);
                topics[*topic_count].phrase[len] = '\0';
                topics[*topic_count].phrase_len = len;
                (*topic_count)++;
            }
            continue;
        }
        if (isupper((unsigned char)*p)) {
            const char *start = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_' || *p == '-'))
                p++;
            if (p > start && (size_t)(p - start) >= 2 &&
                (size_t)(p - start) < SC_CALLBACK_TOPIC_BUF - 1) {
                size_t len = (size_t)(p - start);
                memcpy(topics[*topic_count].phrase, start, len);
                topics[*topic_count].phrase[len] = '\0';
                topics[*topic_count].phrase_len = len;
                (*topic_count)++;
            }
            continue;
        }
        p++;
    }
}

static bool topic_in_recent(const sc_callback_topic_t *t, const sc_channel_history_entry_t *entries,
                            size_t count, size_t recent_start) {
    for (size_t i = recent_start; i < count; i++) {
        const char *text = entries[i].text;
        size_t len = strlen(text);
        for (size_t j = 0; j + t->phrase_len <= len; j++) {
            if (strncasecmp(text + j, t->phrase, t->phrase_len) != 0)
                continue;
            char before = (j > 0) ? (char)tolower((unsigned char)text[j - 1]) : ' ';
            char after = (j + t->phrase_len < len)
                             ? (char)tolower((unsigned char)text[j + t->phrase_len])
                             : ' ';
            if (!isalnum((unsigned char)before) && !isalnum((unsigned char)after))
                return true;
        }
    }
    return false;
}

char *sc_conversation_build_callback(sc_allocator_t *alloc,
                                     const sc_channel_history_entry_t *entries, size_t count,
                                     size_t *out_len) {
    if (!alloc || !out_len)
        return NULL;
    *out_len = 0;
    if (!entries || count < 6)
        return NULL;

    /* ~20% probability: use hash of last message as seed (avoids count%5 restriction) */
    const sc_channel_history_entry_t *last = &entries[count - 1];
    uint32_t hash = 0;
    for (size_t i = 0; i < 20 && i < strlen(last->text); i++)
        hash = hash * 31u + (uint32_t)(unsigned char)last->text[i];
    if (hash % 5u != 0)
        return NULL;

    size_t half = count / 2;
    size_t recent_start = count > 3 ? count - 3 : 0;

    sc_callback_topic_t all_topics[SC_CALLBACK_MAX_TOPICS];
    size_t num_topics = 0;
    memset(all_topics, 0, sizeof(all_topics));

    for (size_t i = 0; i < half && num_topics < SC_CALLBACK_MAX_TOPICS; i++) {
        const char *text = entries[i].text;
        size_t tl = strlen(text);
        sc_callback_topic_t local[8];
        size_t nlocal = 0;
        memset(local, 0, sizeof(local));
        extract_topics_from_text(text, tl, local, &nlocal);
        for (size_t k = 0; k < nlocal && num_topics < SC_CALLBACK_MAX_TOPICS; k++) {
            bool found = false;
            for (size_t j = 0; j < num_topics; j++) {
                if (all_topics[j].phrase_len == local[k].phrase_len &&
                    strncasecmp(all_topics[j].phrase, local[k].phrase, local[k].phrase_len) == 0) {
                    all_topics[j].last_idx = i;
                    all_topics[j].turn_count++;
                    if (strchr(text, '?'))
                        all_topics[j].has_unresolved_question = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                all_topics[num_topics] = local[k];
                all_topics[num_topics].first_idx = i;
                all_topics[num_topics].last_idx = i;
                all_topics[num_topics].turn_count = 1;
                all_topics[num_topics].has_unresolved_question = (strchr(text, '?') != NULL);
                num_topics++;
            }
        }
    }

    sc_callback_topic_t *best = NULL;
    int best_score = SC_CALLBACK_SCORE_MIN - 1;
    for (size_t i = 0; i < num_topics; i++) {
        if (topic_in_recent(&all_topics[i], entries, count, recent_start))
            continue;
        if (all_topics[i].phrase_len < 2)
            continue;
        int score = (int)all_topics[i].turn_count * 2;
        if (all_topics[i].has_unresolved_question)
            score += 3;
        size_t msgs_ago = count - 1 - all_topics[i].last_idx;
        if (msgs_ago < 4)
            score -= 2;
        else if (msgs_ago >= 6)
            score += 1;
        all_topics[i].score = score;
        if (score > best_score) {
            best_score = score;
            best = &all_topics[i];
        }
    }

    if (!best || best_score < SC_CALLBACK_SCORE_MIN)
        return NULL;

    size_t msgs_ago = count - 1 - best->last_idx;
    char buf[512];
    int w = snprintf(buf, sizeof(buf),
                     "\n### Thread Callback Opportunity\n"
                     "An earlier topic that could be naturally revisited:\n"
                     "Topic: \"%.*s\"\n"
                     "Last discussed: %zu messages ago\n"
                     "Consider: \"oh wait, going back to %.*s...\" or weave it in naturally\n"
                     "Only use this if it fits the current flow — don't force it.\n",
                     (int)best->phrase_len, best->phrase, msgs_ago, (int)best->phrase_len,
                     best->phrase);
    if (w <= 0 || (size_t)w >= sizeof(buf))
        return NULL;

    char *result = sc_strndup(alloc, buf, (size_t)w);
    if (result)
        *out_len = (size_t)w;
    return result;
}

/* Data-driven conversation metrics (single O(n) pass over entries) */
typedef struct {
    size_t total_msgs;
    size_t their_msgs;
    size_t their_total_chars;
    size_t their_recent_n;
    size_t their_recent_chars;
    size_t exchanges;
    size_t first_half_chars;
    size_t first_half_n;
    size_t second_half_chars;
    size_t second_half_n;
    size_t exclamation_count;
    size_t question_count;
    size_t longest_their_msg;
    size_t msgs_all_lower;
    size_t msgs_no_period_end;
    size_t rapid_exchanges; /* direction changes in last 6 msgs */
    bool has_link;
    bool recent_shorter_than_earlier;
    char repeated_word[32];
    int repeated_word_hits;
} sc_convo_metrics_t;

static void compute_convo_metrics(const sc_channel_history_entry_t *entries, size_t count,
                                  sc_convo_metrics_t *m) {
    memset(m, 0, sizeof(*m));
    if (!entries || count == 0)
        return;
    m->total_msgs = count;
    size_t mid = count / 2;
    size_t recent_start = count > 6 ? count - 6 : 0;
    bool prev_from_me = entries[recent_start].from_me;

    for (size_t i = 0; i < count; i++) {
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        if (i == 0 || entries[i].from_me != prev_from_me) {
            m->exchanges++;
            if (i >= recent_start)
                m->rapid_exchanges++;
        }
        prev_from_me = entries[i].from_me;

        if (!entries[i].from_me && tl > 2) {
            m->their_msgs++;
            m->their_total_chars += tl;
            if (tl > m->longest_their_msg)
                m->longest_their_msg = tl;
            if (i >= mid) {
                m->second_half_chars += tl;
                m->second_half_n++;
            } else {
                m->first_half_chars += tl;
                m->first_half_n++;
            }
            if (i >= count - 3) {
                m->their_recent_n++;
                m->their_recent_chars += tl;
            }
            for (size_t j = 0; j < tl; j++) {
                if (t[j] == '!')
                    m->exclamation_count++;
                if (t[j] == '?')
                    m->question_count++;
            }
            bool has_upper = false;
            for (size_t j = 0; j < tl; j++) {
                if (t[j] >= 'A' && t[j] <= 'Z') {
                    has_upper = true;
                    break;
                }
            }
            if (!has_upper && tl > 0)
                m->msgs_all_lower++;
            char last_c = t[tl - 1];
            if (last_c != '.' && last_c != '!' && last_c != '?')
                m->msgs_no_period_end++;
            if (tl >= 4 && (memcmp(t, "http", 4) == 0 || strstr(t, ".com") != NULL))
                m->has_link = true;
        }
    }
    if (m->first_half_n > 0 && m->second_half_n > 0) {
        size_t avg1 = m->first_half_chars / m->first_half_n;
        size_t avg2 = m->second_half_chars / m->second_half_n;
        m->recent_shorter_than_earlier = (avg1 > 40 && avg2 < avg1 / 2);
    }
    /* Word frequency for repeated theme (words 5+ chars, no stopword list) */
    {
        typedef struct {
            char word[32];
            int hits;
        } wf_t;
        wf_t freq[16];
        size_t fn = 0;
        memset(freq, 0, sizeof(freq));
        for (size_t i = 0; i < count && fn < 16; i++) {
            if (entries[i].from_me)
                continue;
            const char *p = entries[i].text;
            size_t tl = strlen(p);
            size_t wi = 0;
            while (wi < tl) {
                while (wi < tl && (p[wi] == ' ' || p[wi] == '\n'))
                    wi++;
                size_t ws = wi;
                while (wi < tl && p[wi] != ' ' && p[wi] != '\n')
                    wi++;
                size_t wlen = wi - ws;
                if (wlen < 5 || wlen > 30)
                    continue;
                char word[32];
                for (size_t k = 0; k < wlen && k < 31; k++) {
                    word[k] = (char)tolower((unsigned char)p[ws + k]);
                }
                word[wlen < 31 ? wlen : 31] = '\0';
                bool found = false;
                for (size_t f = 0; f < fn; f++) {
                    if (strcmp(freq[f].word, word) == 0) {
                        freq[f].hits++;
                        found = true;
                        break;
                    }
                }
                if (!found && fn < 16) {
                    memcpy(freq[fn].word, word, wlen + 1);
                    freq[fn].hits = 1;
                    fn++;
                }
            }
        }
        for (size_t f = 0; f < fn; f++) {
            if (freq[f].hits >= 3 && freq[f].hits > m->repeated_word_hits) {
                m->repeated_word_hits = freq[f].hits;
                strncpy(m->repeated_word, freq[f].word, sizeof(m->repeated_word) - 1);
                m->repeated_word[sizeof(m->repeated_word) - 1] = '\0';
            }
        }
    }
}

static const char *day_name(int wday) {
    static const char *days[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday"};
    return (wday >= 0 && wday < 7) ? days[wday] : "?";
}

char *sc_conversation_build_awareness(sc_allocator_t *alloc,
                                      const sc_channel_history_entry_t *entries, size_t count,
                                      size_t *out_len) {
    if (!alloc || !out_len)
        return NULL;
    *out_len = 0;
    if (!entries || count == 0)
        return NULL;

    char *buf = (char *)alloc->alloc(alloc->ctx, CTX_BUF_CAP);
    if (!buf)
        return NULL;

    size_t pos = 0;
    int w;

    /* ── Conversation thread ─────────────────────────────────────────── */
    w = snprintf(buf + pos, CTX_BUF_CAP - pos, "\n--- Recent conversation thread ---\n");
    POS_ADVANCE(w, pos, CTX_BUF_CAP);

    for (size_t i = 0; i < count; i++) {
        const char *who = entries[i].from_me ? "You" : "Them";
        w = snprintf(buf + pos, CTX_BUF_CAP - pos, "[%s] %s: %s\n", entries[i].timestamp, who,
                     entries[i].text);
        POS_ADVANCE(w, pos, CTX_BUF_CAP);
    }

    w = snprintf(buf + pos, CTX_BUF_CAP - pos, "--- End of recent thread ---\n\n");
    POS_ADVANCE(w, pos, CTX_BUF_CAP);

    /* ── Data-driven conversation awareness ─────────────────────────── */
    {
        sc_convo_metrics_t m;
        compute_convo_metrics(entries, count, &m);

        w = snprintf(buf + pos, CTX_BUF_CAP - pos, "--- Conversation awareness ---\n");
        POS_ADVANCE(w, pos, CTX_BUF_CAP);

        /* Message count and length metrics */
        if (m.their_msgs > 0) {
            size_t avg = m.their_total_chars / m.their_msgs;
            const char *depth = (avg < 30)   ? "brief texting"
                                : (avg < 80) ? "moderate-depth texting"
                                             : "longer-form messages";
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- %zu messages exchanged, averaging %zu chars — %s\n", m.total_msgs, avg,
                         depth);
            POS_ADVANCE(w, pos, CTX_BUF_CAP);

            if (m.their_recent_n > 0) {
                size_t recent_avg = m.their_recent_chars / m.their_recent_n;
                if (recent_avg < avg / 2 && avg > 30) {
                    w = snprintf(
                        buf + pos, CTX_BUF_CAP - pos,
                        "- Their last %zu messages averaged %zu chars — getting more brief\n",
                        m.their_recent_n, recent_avg);
                } else if (recent_avg > avg * 2 && m.their_recent_n >= 2) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "- Their last %zu messages averaged %zu chars — opening up more\n",
                                 m.their_recent_n, recent_avg);
                } else {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "- Their last %zu messages averaged %zu chars\n", m.their_recent_n,
                                 recent_avg);
                }
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
        }

        /* Pace (from exchange density) */
        if (m.rapid_exchanges >= 4) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Pace: rapid back-and-forth — active, casual flow\n");
        } else if (m.exchanges >= 6) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Pace: %zu exchanges — steady conversation flow\n", m.exchanges);
        } else if (m.exchanges <= 2) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos, "- Pace: just started — %zu exchange(s)\n",
                         m.exchanges);
        }
        POS_ADVANCE(w, pos, CTX_BUF_CAP);

        /* Time (descriptive only, no behavior prescription) */
        {
            time_t now = time(NULL);
            struct tm lt_buf;
            struct tm *lt = localtime_r(&now, &lt_buf);
            if (lt) {
                int hour = lt->tm_hour;
                int min = lt->tm_min;
                const char *ampm = (hour >= 12) ? "PM" : "AM";
                int h12 = (hour % 12) ? (hour % 12) : 12;
                w = snprintf(buf + pos, CTX_BUF_CAP - pos, "- Time: %d:%02d %s %s\n", h12, min,
                             ampm, day_name(lt->tm_wday));
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
        }

        /* Emotional tone from punctuation (data, not keywords) */
        if (m.exclamation_count >= 3 && m.their_msgs > 0) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Their recent messages carry high exclamation density — excited tone\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        } else if (m.recent_shorter_than_earlier) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Their messages are getting shorter — may be winding down\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }
        if (m.question_count > 0 && m.their_msgs > 0) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- They asked %zu question(s) — answer directly\n", m.question_count);
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }

        /* Conversation arc (data-driven description) */
        if (m.exchanges <= 2) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Flow: %zu exchanges in — conversation just started\n", m.exchanges);
        } else if (m.exchanges <= 6) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Flow: %zu exchanges in — energy building, good for deeper engagement\n",
                         m.exchanges);
        } else if (m.recent_shorter_than_earlier) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Flow: %zu exchanges in — winding down, they're getting briefer\n",
                         m.exchanges);
        } else {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Flow: %zu exchanges in — well into the conversation\n", m.exchanges);
        }
        POS_ADVANCE(w, pos, CTX_BUF_CAP);

        /* Verbosity guidance (descriptive) */
        if (m.their_msgs > 0) {
            size_t avg = m.their_total_chars / m.their_msgs;
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Their messages average %zu characters. Match their brevity.\n", avg);
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }

        /* Repeated theme (data-driven) */
        if (m.repeated_word_hits >= 3) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- They've mentioned \"%s\" %d times — it matters to them\n",
                         m.repeated_word, m.repeated_word_hits);
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }

        /* Link share (structural: contains http/.com) */
        if (m.has_link) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- They shared a link — acknowledge or comment on it\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }

        /* Topic deflection (structural pattern: Q from them, A from you, new topic from them) */
        if (count >= 3) {
            for (size_t i = 2; i < count; i++) {
                if (!entries[i - 2].from_me && entries[i - 1].from_me && !entries[i].from_me) {
                    const char *q = entries[i - 2].text;
                    const char *r = entries[i].text;
                    bool had_q = (strchr(q, '?') != NULL);
                    if (had_q && strlen(r) > 10) {
                        bool shares = false;
                        for (size_t j = 0; j + 4 < strlen(q); j++) {
                            char cq[6];
                            for (int k = 0; k < 5 && j + (size_t)k < strlen(q); k++)
                                cq[k] = (char)tolower((unsigned char)q[j + k]);
                            cq[5] = '\0';
                            for (size_t k = 0; k + 4 < strlen(r); k++) {
                                char cr[6];
                                for (int l = 0; l < 5 && k + (size_t)l < strlen(r); l++)
                                    cr[l] = (char)tolower((unsigned char)r[k + l]);
                                cr[5] = '\0';
                                if (strcmp(cq, cr) == 0) {
                                    shares = true;
                                    break;
                                }
                            }
                            if (shares)
                                break;
                        }
                        if (!shares) {
                            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                         "- They may have shifted topic — follow their lead\n");
                            POS_ADVANCE(w, pos, CTX_BUF_CAP);
                            break;
                        }
                    }
                }
            }
        }

        w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                     "\nUse this context naturally. Reference specific details they mentioned. "
                     "Do NOT summarize or acknowledge this context aloud.\n");
        POS_ADVANCE(w, pos, CTX_BUF_CAP);
    }

    /* Anti-AI style: prompt-level guidance so LLM produces natural output the first time */
    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                 "\n--- CRITICAL STYLE RULES ---\n"
                 "Never start with \"I'd be happy to\", \"Great question!\", \"That's a great "
                 "question\", \"Feel free to\", \"Don't hesitate to\", \"I appreciate\", "
                 "\"Let me know if\", or similar AI-sounding openers. Never say \"As an AI\" or "
                 "\"As a language model\". Just respond naturally like a real person texting.\n");
    POS_ADVANCE(w, pos, CTX_BUF_CAP);

    /* Situational length calibration from the last incoming message */
    if (entries && count > 0) {
        const char *last_their_msg = NULL;
        size_t last_their_len = 0;
        for (size_t i = count; i > 0; i--) {
            if (!entries[i - 1].from_me) {
                last_their_msg = entries[i - 1].text;
                last_their_len = strlen(last_their_msg);
                break;
            }
        }
        if (last_their_msg && last_their_len > 0) {
            size_t cal_len = sc_conversation_calibrate_length(
                last_their_msg, last_their_len, entries, count, buf + pos, CTX_BUF_CAP - pos);
            pos += cal_len;
        }
    }

    buf[pos] = '\0';
    *out_len = pos;
    return buf;
}

/* ── Conversation Quality Evaluator (qualitative, context-driven) ─────── */

static void compute_their_avg_len(const sc_channel_history_entry_t *entries, size_t count,
                                  size_t *out_avg, size_t *out_recent_avg) {
    *out_avg = 0;
    *out_recent_avg = 0;
    if (!entries || count == 0)
        return;
    size_t total_chars = 0;
    size_t their_n = 0;
    size_t recent_chars = 0;
    size_t recent_n = 0;
    size_t recent_start = count > 4 ? count - 4 : 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            continue;
        size_t tl = strlen(entries[i].text);
        if (tl > 0) {
            total_chars += tl;
            their_n++;
            if (i >= recent_start) {
                recent_chars += tl;
                recent_n++;
            }
        }
    }
    if (their_n > 0)
        *out_avg = total_chars / their_n;
    if (recent_n > 0)
        *out_recent_avg = recent_chars / recent_n;
}

sc_quality_score_t sc_conversation_evaluate_quality(const char *response, size_t response_len,
                                                    const sc_channel_history_entry_t *entries,
                                                    size_t count, uint32_t max_chars) {
    sc_quality_score_t score = {0, 0, 0, 0, 0, false, {0}};
    if (!response || response_len == 0)
        return score;

    size_t their_avg = 0;
    size_t their_recent_avg = 0;
    compute_their_avg_len(entries, count, &their_avg, &their_recent_avg);
    size_t ref_len = their_recent_avg > 0 ? their_recent_avg : (their_avg > 0 ? their_avg : 50);
    if (ref_len < 10)
        ref_len = 10;

    /* Brevity (0-25): ratio of response to their average length */
    double ratio = (double)response_len / (double)ref_len;
    if (ratio <= 1.5)
        score.brevity = 25;
    else if (ratio <= 3.0)
        score.brevity = 20;
    else if (ratio <= 6.0)
        score.brevity = 10;
    else
        score.brevity = 0;
    if (max_chars > 0 && response_len > max_chars * 2)
        score.brevity = 0;

    /* Validation (0-25): does response reflect their energy? Use structural cues. */
    if (entries && count > 0) {
        size_t their_excl = 0;
        size_t their_q = 0;
        for (size_t i = count > 3 ? count - 3 : 0; i < count; i++) {
            if (entries[i].from_me)
                continue;
            const char *t = entries[i].text;
            for (size_t j = 0; t[j]; j++) {
                if (t[j] == '!')
                    their_excl++;
                if (t[j] == '?')
                    their_q++;
            }
        }
        int resp_excl = 0;
        for (size_t k = 0; k < response_len; k++) {
            if (response[k] == '!')
                resp_excl++;
        }
        if (their_excl >= 2 && resp_excl > 0)
            score.validation = 25;
        else if (their_q > 0 && response_len > 5)
            score.validation = 20;
        else
            score.validation = 18;
    } else {
        score.validation = 20;
    }

    /* Warmth (0-25): structural (exclamation, personal tone) vs robotic tells */
    int warmth = 15;
    if (strchr(response, '!'))
        warmth += 5;
    if (str_contains_ci(response, response_len, "as an AI") ||
        str_contains_ci(response, response_len, "as a language model"))
        warmth -= 20;
    else if (str_contains_ci(response, response_len, "I'd be happy to") ||
             str_contains_ci(response, response_len, "let me know if") ||
             str_contains_ci(response, response_len, "feel free") ||
             str_contains_ci(response, response_len, "certainly"))
        warmth -= 18;
    if (warmth < 0)
        warmth = 0;
    if (warmth > 25)
        warmth = 25;
    score.warmth = warmth;

    /* Naturalness (0-25): structural (markdown, lists, AI tells) */
    int nat = 20;
    if (str_contains_ci(response, response_len, "**") ||
        str_contains_ci(response, response_len, "##") ||
        str_contains_ci(response, response_len, "```"))
        nat -= 12;
    else if (str_contains_ci(response, response_len, "- ") ||
             str_contains_ci(response, response_len, "1. "))
        nat -= 3;
    if (strchr(response, ';'))
        nat -= 5;
    if (strstr(response, " — ") || strstr(response, " - "))
        nat -= 3;
    int excl_count = 0;
    for (size_t k = 0; k < response_len; k++) {
        if (response[k] == '!')
            excl_count++;
    }
    if (excl_count > 2 && response_len < 200)
        nat -= 5;
    if (nat < 0)
        nat = 0;
    if (nat > 25)
        nat = 25;
    score.naturalness = nat;

    score.total = score.brevity + score.validation + score.warmth + score.naturalness;

    /* needs_revision only on gross mismatches (10x length, etc.) */
    bool gross_length = (ratio > 10.0 || (ratio < 0.1 && response_len > 5));
    bool gross_structural = (score.warmth < 5 || score.naturalness < 5);
    score.needs_revision = gross_length || gross_structural;

    if (score.needs_revision && ratio > 5.0 && their_avg > 0) {
        int n = snprintf(score.guidance, sizeof(score.guidance),
                         "Your response was %zu chars but their last messages averaged %zu chars. "
                         "Tighten up significantly. Match their energy.",
                         response_len, their_avg);
        if (n <= 0 || (size_t)n >= sizeof(score.guidance))
            score.guidance[0] = '\0';
    } else if (score.needs_revision && ratio < 0.2 && response_len > 50) {
        snprintf(
            score.guidance, sizeof(score.guidance),
            "Your response was much shorter than their typical depth. Consider adding a bit more.");
    } else if (score.needs_revision && gross_structural) {
        if (score.warmth < 5 && score.naturalness < 5) {
            snprintf(score.guidance, sizeof(score.guidance),
                     "Your response felt distant and formal. Drop the formality, show you care.");
        } else if (score.warmth < 5) {
            snprintf(score.guidance, sizeof(score.guidance),
                     "Your response felt distant. Show you care.");
        } else if (score.naturalness < 5) {
            snprintf(score.guidance, sizeof(score.guidance),
                     "Your phrasing felt formal. Drop the formality.");
        } else {
            snprintf(score.guidance, sizeof(score.guidance),
                     "Response has AI-sounding or structural tells. Rewrite naturally.");
        }
    } else {
        score.guidance[0] = '\0';
    }
    return score;
}

/* ── Honesty Guardrail (pattern-based, contextual output) ───────────────── */

/* Detect if user is asking about a commitment or action: question + action-query pattern.
 * Lightweight: question mark + "did you" or "have you" (covers did you call, have you sent, etc.)
 */
static bool detect_action_commitment_query(const char *message, size_t message_len) {
    bool has_question = false;
    for (size_t i = 0; i < message_len; i++) {
        if (message[i] == '?') {
            has_question = true;
            break;
        }
    }
    if (!has_question)
        return false;
    return str_contains_ci(message, message_len, "did you") ||
           str_contains_ci(message, message_len, "have you");
}

char *sc_conversation_honesty_check(sc_allocator_t *alloc, const char *message,
                                    size_t message_len) {
    if (!alloc || !message || message_len == 0)
        return NULL;

    if (!detect_action_commitment_query(message, message_len))
        return NULL;

    char *buf = (char *)alloc->alloc(alloc->ctx, 512);
    if (!buf)
        return NULL;

    int n = snprintf(buf, 512,
                     "HONESTY GUARDRAIL: The user appears to be asking about something you "
                     "committed to or were expected to do. Be honest about what actually happened. "
                     "Don't deflect, don't be vague. If you don't know or haven't done it, say so "
                     "directly. Never fabricate completed actions.");
    if (n <= 0 || (size_t)n >= 512) {
        alloc->free(alloc->ctx, buf, 512);
        return NULL;
    }
    return buf;
}

/* ── Narrative Arc Detection ────────────────────────────────────────── */

sc_narrative_phase_t sc_conversation_detect_narrative(const sc_channel_history_entry_t *entries,
                                                      size_t count) {
    if (!entries || count == 0)
        return SC_NARRATIVE_OPENING;

    /* Count exchanges (direction changes) */
    size_t exchanges = 0;
    bool last_from_me = false;
    for (size_t i = 0; i < count; i++) {
        if (i == 0 || entries[i].from_me != last_from_me)
            exchanges++;
        last_from_me = entries[i].from_me;
    }

    /* Measure emotional intensity in recent messages */
    int emotional_words = 0;
    int question_marks __attribute__((unused)) = 0;
    int exclamation_marks = 0;
    size_t recent_start = count > 5 ? count - 5 : 0;
    for (size_t i = recent_start; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        for (size_t j = 0; j < tl; j++) {
            if (t[j] == '?')
                question_marks++;
            if (t[j] == '!')
                exclamation_marks++;
        }
        if (str_contains_ci(t, tl, "love") || str_contains_ci(t, tl, "hate") ||
            str_contains_ci(t, tl, "scared") || str_contains_ci(t, tl, "angry") ||
            str_contains_ci(t, tl, "can't believe") || str_contains_ci(t, tl, "so happy") ||
            str_contains_ci(t, tl, "so sad") || str_contains_ci(t, tl, "hurt") ||
            str_contains_ci(t, tl, "need to tell") || str_contains_ci(t, tl, "important"))
            emotional_words++;
    }

    /* Detect closing signals */
    if (count >= 2) {
        const char *last = entries[count - 1].text;
        size_t ll = strlen(last);
        if (!entries[count - 1].from_me &&
            (str_contains_ci(last, ll, "gotta go") || str_contains_ci(last, ll, "talk later") ||
             str_contains_ci(last, ll, "bye") || str_contains_ci(last, ll, "night") ||
             str_contains_ci(last, ll, "ttyl") || str_contains_ci(last, ll, "heading out") ||
             (ll < 10 && (str_contains_ci(last, ll, "ok") || str_contains_ci(last, ll, "k")))))
            return SC_NARRATIVE_CLOSING;
    }

    /* Map to phases */
    if (exchanges <= 2)
        return SC_NARRATIVE_OPENING;
    if (exchanges <= 5)
        return SC_NARRATIVE_BUILDING;
    if (emotional_words >= 2 || exclamation_marks >= 3)
        return SC_NARRATIVE_PEAK;
    if (exchanges > 5 && emotional_words >= 1)
        return SC_NARRATIVE_APPROACHING_CLIMAX;
    if (exchanges > 10)
        return SC_NARRATIVE_RELEASE;
    return SC_NARRATIVE_BUILDING;
}

/* ── Engagement Scoring ─────────────────────────────────────────────── */

sc_engagement_level_t sc_conversation_detect_engagement(const sc_channel_history_entry_t *entries,
                                                        size_t count) {
    if (!entries || count == 0)
        return SC_ENGAGEMENT_MODERATE;

    size_t their_msgs = 0;
    size_t total_their_len = 0;
    size_t very_short = 0;
    size_t questions_to_us = 0;
    size_t recent = count > 6 ? count - 6 : 0;

    for (size_t i = recent; i < count; i++) {
        if (entries[i].from_me)
            continue;
        their_msgs++;
        size_t tl = strlen(entries[i].text);
        total_their_len += tl;
        if (tl < 8)
            very_short++;
        for (size_t j = 0; j < tl; j++) {
            if (entries[i].text[j] == '?') {
                questions_to_us++;
                break;
            }
        }
    }

    if (their_msgs == 0)
        return SC_ENGAGEMENT_DISTRACTED;

    size_t avg_len = total_their_len / their_msgs;

    /* High: asking questions, longer messages, engaged */
    if (questions_to_us >= 2 || avg_len > 60)
        return SC_ENGAGEMENT_HIGH;

    /* Low: very short responses, no questions */
    if (avg_len < 15 && very_short >= 2 && questions_to_us == 0)
        return SC_ENGAGEMENT_LOW;

    /* Distracted: single-word or empty responses */
    if (their_msgs >= 2 && very_short == their_msgs)
        return SC_ENGAGEMENT_DISTRACTED;

    return SC_ENGAGEMENT_MODERATE;
}

/* ── Emotional State Detection ──────────────────────────────────────── */

sc_emotional_state_t sc_conversation_detect_emotion(const sc_channel_history_entry_t *entries,
                                                    size_t count) {
    sc_emotional_state_t state = {0.0f, 0.0f, false, "neutral"};
    if (!entries || count == 0)
        return state;

    float positive = 0, negative = 0;
    int samples = 0;
    size_t start = count > 8 ? count - 8 : 0;

    for (size_t i = start; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        samples++;

        /* Positive signals */
        if (str_contains_ci(t, tl, "happy"))
            positive += 1.0f;
        if (str_contains_ci(t, tl, "excited"))
            positive += 1.2f;
        if (str_contains_ci(t, tl, "love"))
            positive += 0.8f;
        if (str_contains_ci(t, tl, "amazing"))
            positive += 1.0f;
        if (str_contains_ci(t, tl, "great"))
            positive += 0.6f;
        if (str_contains_ci(t, tl, "awesome"))
            positive += 0.8f;
        if (str_contains_ci(t, tl, "lol") || str_contains_ci(t, tl, "haha"))
            positive += 0.4f;
        if (str_contains_ci(t, tl, "yay") || str_contains_ci(t, tl, "yes!"))
            positive += 0.7f;

        /* Negative signals */
        if (str_contains_ci(t, tl, "sad"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "depressed"))
            negative += 1.5f;
        if (str_contains_ci(t, tl, "stressed"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "worried"))
            negative += 0.8f;
        if (str_contains_ci(t, tl, "anxious"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "angry"))
            negative += 1.2f;
        if (str_contains_ci(t, tl, "frustrated"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "hurt"))
            negative += 1.2f;
        if (str_contains_ci(t, tl, "scared"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "lonely"))
            negative += 1.3f;
        if (str_contains_ci(t, tl, "crying") || str_contains_ci(t, tl, "cry"))
            negative += 1.5f;
        if (str_contains_ci(t, tl, "hate"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "ugh") || str_contains_ci(t, tl, "damn"))
            negative += 0.5f;
    }

    if (samples == 0)
        return state;

    state.valence = (positive - negative) / (float)samples;
    state.intensity = (positive + negative) / (float)samples;
    if (state.valence < -0.5f)
        state.valence = -1.0f;
    if (state.valence > 1.0f)
        state.valence = 1.0f;
    if (state.intensity > 2.0f)
        state.intensity = 2.0f;

    if (negative > positive * 1.5f) {
        state.concerning = negative >= 2.0f;
        if (str_contains_ci(entries[count - 1].text, strlen(entries[count - 1].text),
                            "depressed") ||
            str_contains_ci(entries[count - 1].text, strlen(entries[count - 1].text), "crying"))
            state.dominant_emotion = "distressed";
        else if (negative >= 1.5f)
            state.dominant_emotion = "upset";
        else
            state.dominant_emotion = "down";
    } else if (positive > negative * 1.5f) {
        if (positive >= 2.0f)
            state.dominant_emotion = "excited";
        else
            state.dominant_emotion = "positive";
    } else if (state.intensity > 0.3f) {
        state.dominant_emotion = "mixed";
    }

    return state;
}

/* ── Typo correction fragment (*meant) ─────────────────────────────────── */

size_t sc_conversation_generate_correction(const char *original, size_t original_len,
                                           const char *typo_applied, size_t typo_applied_len,
                                           char *out_buf, size_t out_cap, uint32_t seed,
                                           uint32_t correction_chance) {
    if (!original || !typo_applied || !out_buf || out_cap < 4)
        return 0;

    /* Find first difference */
    size_t diff_pos = 0;
    size_t lim = original_len < typo_applied_len ? original_len : typo_applied_len;
    while (diff_pos < lim && original[diff_pos] == typo_applied[diff_pos])
        diff_pos++;

    /* No difference: identical strings */
    if (diff_pos >= lim && original_len == typo_applied_len)
        return 0;

    /* Find word boundaries in original: word = run of non-space chars bounded by space or start/end
     */
    size_t word_start = diff_pos;
    while (word_start > 0 && original[word_start - 1] != ' ')
        word_start--;
    size_t word_end = diff_pos;
    while (word_end < original_len && original[word_end] != ' ')
        word_end++;

    size_t word_len = word_end - word_start;
    if (word_len == 0)
        return 0;

    /* Seed-based PRNG: LCG for deterministic chance check */
    uint32_t state = seed * 1103515245u + 12345u;
    uint32_t roll = (state >> 16) % 100u;
    if (roll >= correction_chance)
        return 0;

    /* Format "*<correct_word>" — need 1 + word_len + 1 for null */
    if (out_cap < 2 + word_len)
        word_len = out_cap > 2 ? out_cap - 2 : 0;
    if (word_len == 0)
        return 0;

    int n = snprintf(out_buf, out_cap, "*%.*s", (int)word_len, original + word_start);
    if (n < 0)
        return 0;
    return (size_t)((n >= (int)out_cap) ? out_cap - 1 : n);
}

/* ── Multi-message splitting ──────────────────────────────────────────── */

/*
 * Split at natural breakpoints that mimic how humans fragment thoughts
 * across multiple iMessage bubbles. Priorities:
 * 1. Explicit newlines in the response
 * 2. Sentence boundaries followed by conjunctions/interjections
 * 3. Sentence boundaries when response is long enough
 */

static bool is_split_starter(const char *s, size_t len) {
    if (len < 2)
        return false;
    /* Starters that signal a new thought bubble */
    static const char *starters[] = {
        "oh ",  "but ", "and ", "like ",   "also ", "wait ", "haha", "lol", "omg",
        "ngl ", "tbh ", "btw ", "anyway ", "ok ",   "so ",   "yeah", "nah", NULL,
    };
    for (int i = 0; starters[i]; i++) {
        size_t sl = strlen(starters[i]);
        if (len >= sl) {
            bool match = true;
            for (size_t j = 0; j < sl; j++) {
                char a = s[j];
                char b = starters[i][j];
                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
    }
    return false;
}

size_t sc_conversation_split_response(sc_allocator_t *alloc, const char *response,
                                      size_t response_len, sc_message_fragment_t *fragments,
                                      size_t max_fragments) {
    if (!alloc || !response || response_len == 0 || !fragments || max_fragments == 0)
        return 0;

    /* Short responses stay as one message */
    if (response_len < 40 || max_fragments == 1) {
        char *copy = (char *)alloc->alloc(alloc->ctx, response_len + 1);
        if (!copy)
            return 0;
        memcpy(copy, response, response_len);
        copy[response_len] = '\0';
        fragments[0].text = copy;
        fragments[0].text_len = response_len;
        fragments[0].delay_ms = 0;
        return 1;
    }

    /* First pass: find split points */
    size_t split_points[8];
    size_t split_count = 0;

    /* Check for explicit newlines first */
    for (size_t i = 1; i < response_len - 1 && split_count < 7; i++) {
        if (response[i] == '\n') {
            /* Skip consecutive newlines */
            size_t next = i + 1;
            while (next < response_len && response[next] == '\n')
                next++;
            if (next < response_len && next - i > 0) {
                split_points[split_count++] = next;
                i = next - 1;
            }
        }
    }

    /* If no newlines, split at sentence boundaries before conjunctions */
    if (split_count == 0) {
        for (size_t i = 2; i < response_len - 2 && split_count < 7; i++) {
            char prev = response[i - 1];
            if ((prev == '.' || prev == '!' || prev == '?') && response[i] == ' ') {
                size_t remaining = response_len - (i + 1);
                if (remaining > 10 && is_split_starter(response + i + 1, remaining)) {
                    split_points[split_count++] = i + 1;
                }
            }
        }
    }

    /* If still nothing, split at sentence boundaries for long responses */
    if (split_count == 0 && response_len > 80) {
        for (size_t i = 30; i < response_len - 15 && split_count < 3; i++) {
            char prev = response[i - 1];
            if ((prev == '.' || prev == '!' || prev == '?') && response[i] == ' ') {
                split_points[split_count++] = i + 1;
            }
        }
    }

    if (split_count == 0) {
        char *copy = (char *)alloc->alloc(alloc->ctx, response_len + 1);
        if (!copy)
            return 0;
        memcpy(copy, response, response_len);
        copy[response_len] = '\0';
        fragments[0].text = copy;
        fragments[0].text_len = response_len;
        fragments[0].delay_ms = 0;
        return 1;
    }

    /* Cap at max_fragments - 1 split points */
    if (split_count >= max_fragments)
        split_count = max_fragments - 1;

    /* Build fragments */
    size_t frag_count = 0;
    size_t start = 0;
    for (size_t s = 0; s < split_count && frag_count < max_fragments - 1; s++) {
        size_t end = split_points[s];
        /* Trim trailing whitespace/newlines */
        size_t trim_end = end;
        while (trim_end > start &&
               (response[trim_end - 1] == ' ' || response[trim_end - 1] == '\n'))
            trim_end--;
        size_t flen = trim_end - start;
        if (flen < 2) {
            start = end;
            continue;
        }
        char *frag = (char *)alloc->alloc(alloc->ctx, flen + 1);
        if (!frag) {
            for (size_t k = 0; k < frag_count; k++)
                alloc->free(alloc->ctx, fragments[k].text, fragments[k].text_len + 1);
            return 0;
        }
        memcpy(frag, response + start, flen);
        frag[flen] = '\0';
        fragments[frag_count].text = frag;
        fragments[frag_count].text_len = flen;
        fragments[frag_count].delay_ms = frag_count == 0 ? 0 : (uint32_t)(500 + flen * 15);
        if (fragments[frag_count].delay_ms > 3000)
            fragments[frag_count].delay_ms = 3000;
        frag_count++;
        start = end;
    }

    /* Final fragment */
    if (start < response_len) {
        /* Trim leading whitespace */
        while (start < response_len && response[start] == ' ')
            start++;
        size_t flen = response_len - start;
        if (flen > 0) {
            char *frag = (char *)alloc->alloc(alloc->ctx, flen + 1);
            if (!frag) {
                for (size_t k = 0; k < frag_count; k++)
                    alloc->free(alloc->ctx, fragments[k].text, fragments[k].text_len + 1);
                return 0;
            }
            memcpy(frag, response + start, flen);
            frag[flen] = '\0';
            fragments[frag_count].text = frag;
            fragments[frag_count].text_len = flen;
            fragments[frag_count].delay_ms = frag_count == 0 ? 0 : (uint32_t)(500 + flen * 15);
            if (fragments[frag_count].delay_ms > 3000)
                fragments[frag_count].delay_ms = 3000;
            frag_count++;
        }
    }

    return frag_count;
}

/* ── Situational length calibration ───────────────────────────────────── */

/*
 * Instead of "keep under 50 chars", tell the model WHY a certain length
 * is right. Humans calibrate response length by message type, not by
 * counting characters. This function classifies the incoming message
 * and produces a directive that mimics human instinct.
 */

size_t sc_conversation_calibrate_length(const char *last_msg, size_t last_msg_len,
                                        const sc_channel_history_entry_t *entries, size_t count,
                                        char *buf, size_t cap) {
    if (!last_msg || last_msg_len == 0 || !buf || cap < 64)
        return 0;

    size_t pos = 0;
    int w;

    sc_convo_metrics_t m;
    compute_convo_metrics(entries, count, &m);

    w = snprintf(buf + pos, cap - pos, "\n--- Response calibration ---\n");
    POS_ADVANCE(w, pos, cap);

    /* Style calibration from actual conversation data */
    if (m.their_msgs > 0) {
        size_t avg = m.their_total_chars / m.their_msgs;
        w = snprintf(buf + pos, cap - pos,
                     "Their average message: %zu chars across %zu messages.\n", avg, m.their_msgs);
        POS_ADVANCE(w, pos, cap);

        if (m.longest_their_msg > 0) {
            w = snprintf(buf + pos, cap - pos, "Their longest recent message: %zu chars.\n",
                         m.longest_their_msg);
            POS_ADVANCE(w, pos, cap);
        }

        /* Style: capitalization, punctuation */
        if (m.their_msgs >= 2) {
            if (m.msgs_all_lower > m.their_msgs * 2 / 3) {
                w = snprintf(buf + pos, cap - pos,
                             "Their style: mostly lowercase, minimal punctuation.\n");
            } else if (m.msgs_no_period_end > m.their_msgs * 2 / 3) {
                w = snprintf(buf + pos, cap - pos,
                             "Their style: rarely ends with periods, casual punctuation.\n");
            } else {
                w = snprintf(buf + pos, cap - pos,
                             "Their style: mixed capitalization and punctuation.\n");
            }
            POS_ADVANCE(w, pos, cap);
        }
    }

    /* Last message length (structural) */
    w = snprintf(buf + pos, cap - pos, "Their last message: %zu chars. ", last_msg_len);
    POS_ADVANCE(w, pos, cap);
    if (last_msg_len < 15) {
        w = snprintf(buf + pos, cap - pos, "Very brief — match that brevity.\n");
    } else if (last_msg_len > 100) {
        w = snprintf(buf + pos, cap - pos,
                     "Substantial — you can respond with more depth, but don't over-match.\n");
    } else {
        w = snprintf(buf + pos, cap - pos, "Moderate length — match their energy and length.\n");
    }
    POS_ADVANCE(w, pos, cap);

    /* Question (structural: contains ?) */
    if (strchr(last_msg, '?') != NULL) {
        w = snprintf(buf + pos, cap - pos, "They asked a question — answer it directly.\n");
        POS_ADVANCE(w, pos, cap);
    }

    /* Link (structural: contains http or .com) */
    {
        bool last_has_http = (last_msg_len >= 4 && (strncmp(last_msg, "http", 4) == 0 ||
                                                    strstr(last_msg, "http") != NULL ||
                                                    strstr(last_msg, ".com") != NULL));
        if (m.has_link || last_has_http) {
            w = snprintf(buf + pos, cap - pos,
                         "They shared a link — acknowledge or comment on it.\n");
            POS_ADVANCE(w, pos, cap);
        }
    }

    /* Momentum from exchange density */
    if (m.rapid_exchanges >= 4) {
        w = snprintf(
            buf + pos, cap - pos,
            "Rapid-fire exchange — keep responses extra short, one thought per message.\n");
        POS_ADVANCE(w, pos, cap);
    }

    /* Generic guidance (data-driven, not prescriptive) */
    w = snprintf(buf + pos, cap - pos,
                 "Match their energy and length. If they're brief, be brief. If they open up, "
                 "you can too.\n");
    POS_ADVANCE(w, pos, cap);

    w = snprintf(buf + pos, cap - pos, "--- End calibration ---\n");
    POS_ADVANCE(w, pos, cap);

    buf[pos] = '\0';
    return pos;
}

/* ── Texting style analysis ───────────────────────────────────────────── */

char *sc_conversation_analyze_style(sc_allocator_t *alloc,
                                    const sc_channel_history_entry_t *entries, size_t count,
                                    size_t *out_len) {
    if (!alloc || !entries || count == 0 || !out_len)
        return NULL;
    *out_len = 0;

    /* Analyze their (non-self) messages */
    size_t their_count = 0;
    size_t total_chars = 0;
    size_t msgs_with_caps_start = 0;
    size_t msgs_no_period_end = 0;
    size_t msgs_all_lower = 0;
    size_t msgs_with_abbrev = 0;
    size_t fragment_count = 0; /* messages under 25 chars (rapid-fire fragments) */

    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        if (tl < 2)
            continue;
        their_count++;
        total_chars += tl;

        if (tl < 25)
            fragment_count++;

        bool has_upper = false;
        bool has_lower = false;
        bool starts_cap = (t[0] >= 'A' && t[0] <= 'Z');
        if (starts_cap)
            msgs_with_caps_start++;

        char last_alpha = 0;
        for (size_t j = 0; j < tl; j++) {
            if (t[j] >= 'A' && t[j] <= 'Z')
                has_upper = true;
            else if (t[j] >= 'a' && t[j] <= 'z')
                has_lower = true;
            if ((t[j] >= 'a' && t[j] <= 'z') || (t[j] >= 'A' && t[j] <= 'Z'))
                last_alpha = t[j];
        }

        if (has_lower && !has_upper)
            msgs_all_lower++;
        if (last_alpha && last_alpha != '.' && t[tl - 1] != '.' && t[tl - 1] != '!' &&
            t[tl - 1] != '?')
            msgs_no_period_end++;

        /* Abbreviation detection */
        if (str_contains_ci(t, tl, "lol") || str_contains_ci(t, tl, "omg") ||
            str_contains_ci(t, tl, "ngl") || str_contains_ci(t, tl, "tbh") ||
            str_contains_ci(t, tl, "rn") || str_contains_ci(t, tl, "idk") ||
            str_contains_ci(t, tl, "imo") || str_contains_ci(t, tl, "nvm") ||
            str_contains_ci(t, tl, "btw") || str_contains_ci(t, tl, "wya") ||
            str_contains_ci(t, tl, "hbu"))
            msgs_with_abbrev++;
    }

    if (their_count < 3) {
        return NULL;
    }

#define STYLE_BUF_CAP 2048
    char *buf = (char *)alloc->alloc(alloc->ctx, STYLE_BUF_CAP);
    if (!buf)
        return NULL;
    size_t pos = 0;
    int w;

    w = snprintf(buf + pos, STYLE_BUF_CAP - pos, "--- Their texting style (mirror this) ---\n");
    POS_ADVANCE(w, pos, STYLE_BUF_CAP);

    /* Capitalization pattern */
    if (msgs_all_lower > their_count * 2 / 3) {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "CAPS: They almost never capitalize. Write in all lowercase. "
                     "No capital letters at start of sentences.\n");
    } else if (msgs_with_caps_start < their_count / 3) {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "CAPS: They rarely capitalize sentence starts. "
                     "Skip capital letters most of the time.\n");
    } else {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "CAPS: They use normal capitalization. Match that.\n");
    }
    POS_ADVANCE(w, pos, STYLE_BUF_CAP);

    /* Punctuation pattern */
    if (msgs_no_period_end > their_count * 2 / 3) {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "PUNCTUATION: They almost never end with periods. "
                     "Drop periods at end of messages. Questions marks are ok.\n");
    } else {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "PUNCTUATION: They use normal punctuation. Match that.\n");
    }
    POS_ADVANCE(w, pos, STYLE_BUF_CAP);

    /* Fragmentation pattern */
    if (fragment_count > their_count / 2) {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "STYLE: They send short rapid-fire messages. Keep yours very short too. "
                     "One thought per message.\n");
    } else {
        w = 0;
    }
    POS_ADVANCE(w, pos, STYLE_BUF_CAP);

    /* Abbreviation pattern */
    if (msgs_with_abbrev > their_count / 3) {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "SLANG: They use texting abbreviations (lol, omg, ngl, tbh, idk). "
                     "Use these naturally.\n");
        POS_ADVANCE(w, pos, STYLE_BUF_CAP);
    }

    /* Average message length */
    if (their_count > 0) {
        size_t avg = total_chars / their_count;
        if (avg < 25) {
            w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                         "LENGTH: Their average message is ~%zu chars. "
                         "Keep yours under 30 chars per message.\n",
                         avg);
        } else if (avg < 60) {
            w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                         "LENGTH: Their average message is ~%zu chars. "
                         "Keep yours around that length.\n",
                         avg);
        } else {
            w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                         "LENGTH: Their average message is ~%zu chars. "
                         "You can write a bit more but stay concise.\n",
                         avg);
        }
        POS_ADVANCE(w, pos, STYLE_BUF_CAP);
    }

    /* Anti-AI warnings */
    w = snprintf(
        buf + pos, STYLE_BUF_CAP - pos,
        "\n--- Anti-patterns (NEVER do these in texts) ---\n"
        "- Never use semicolons or em-dashes in texts\n"
        "- Never use \"certainly\", \"absolutely\", \"I'd be happy to\", "
        "\"let me know if\", \"feel free\"\n"
        "- Never start with their name (siblings don't address each other by name)\n"
        "- Never use perfect grammar if they don't\n"
        "- Never write more than 2x their average message length\n"
        "- Never use numbered lists or bullet points\n"
        "- Never use \"!\" on every message — save it for when you mean it\n"
        "- Never give unsolicited advice unless they explicitly ask\n"
        "- It's ok to be blunt, sarcastic, or tease — that's how siblings talk\n"
        "- It's ok to just say \"lol\" or \"yeah\" — not everything needs a real response\n");
    POS_ADVANCE(w, pos, STYLE_BUF_CAP);

    buf[pos] = '\0';
    *out_len = pos;
    return buf;
}

/* ── Typing quirk post-processing ─────────────────────────────────────── */

static bool quirk_enabled(const char *const *quirks, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (quirks[i] && strcmp(quirks[i], name) == 0)
            return true;
    }
    return false;
}

size_t sc_conversation_apply_typing_quirks(char *buf, size_t len, const char *const *quirks,
                                           size_t quirks_count) {
    if (!buf || len == 0 || !quirks || quirks_count == 0)
        return len;

    bool do_lowercase = quirk_enabled(quirks, quirks_count, "lowercase");
    bool do_no_periods = quirk_enabled(quirks, quirks_count, "no_periods");
    bool do_no_commas = quirk_enabled(quirks, quirks_count, "no_commas");
    bool do_no_apostrophes = quirk_enabled(quirks, quirks_count, "no_apostrophes");
    bool do_double_space_to_newline =
        quirk_enabled(quirks, quirks_count, "double_space_to_newline");

    if (do_lowercase) {
        for (size_t i = 0; i < len; i++) {
            if (buf[i] >= 'A' && buf[i] <= 'Z')
                buf[i] += 32;
        }
    }

    if (do_no_periods || do_no_commas || do_no_apostrophes) {
        size_t out = 0;
        for (size_t i = 0; i < len; i++) {
            bool strip = false;
            if (do_no_periods && buf[i] == '.') {
                bool is_end = (i + 1 == len) || (buf[i + 1] == ' ' && i + 2 < len &&
                                                 buf[i + 2] >= 'A' && buf[i + 2] <= 'z');
                bool in_ellipsis = (i + 2 < len && buf[i + 1] == '.' && buf[i + 2] == '.') ||
                                   (i > 0 && buf[i - 1] == '.');
                if (is_end && !in_ellipsis)
                    strip = true;
            }
            if (do_no_commas && buf[i] == ',')
                strip = true;
            if (do_no_apostrophes && buf[i] == '\'')
                strip = true;
            if (!strip)
                buf[out++] = buf[i];
        }
        buf[out] = '\0';
        len = out;
    }

    if (do_double_space_to_newline) {
        size_t out = 0;
        for (size_t i = 0; i < len; i++) {
            if (i + 1 < len && buf[i] == ' ' && buf[i + 1] == ' ') {
                buf[out++] = '\n';
                i++; /* skip second space */
            } else {
                buf[out++] = buf[i];
            }
        }
        buf[out] = '\0';
        len = out;
    }

    /* Strip trailing whitespace that may result from punctuation removal */
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\n')) {
        len--;
        buf[len] = '\0';
    }

    return len;
}

/* ── Typo simulation post-processor ──────────────────────────────────── */

/* QWERTY adjacency: for each lowercase letter, neighbors on keyboard.
 * Used for adjacent-key-swap typo. Index by (c - 'a'). */
static const char *const QWERTY_NEIGHBORS[] = {
    "sqwz",   /* a */
    "vghn",   /* b */
    "xdfv",   /* c */
    "serfcx", /* d */
    "wrds",   /* e */
    "drtgvc", /* f */
    "ftyhbv", /* g */
    "gyujnb", /* h */
    "uokj",   /* i */
    "huikmn", /* j */
    "jolm",   /* k */
    "kop",    /* l */
    "njk",    /* m */
    "bhjm",   /* n */
    "iplk",   /* o */
    "ol",     /* p */
    "wa",     /* q */
    "etfd",   /* r */
    "awedxz", /* s */
    "ryfg",   /* t */
    "yihj",   /* u */
    "cfgxb",  /* v */
    "qeas",   /* w */
    "zsdc",   /* x */
    "tugh",   /* y */
    "asx",    /* z */
};

static uint32_t prng_next(uint32_t *seed) {
    *seed = *seed * 1103515245u + 12345u;
    return (*seed >> 16u) & 0x7fffu;
}

static bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

size_t sc_conversation_apply_typos(char *buf, size_t len, size_t cap, uint32_t seed) {
    if (!buf || len == 0)
        return len;
    if (len >= cap)
        return len;

    uint32_t s = seed;
    uint32_t val = prng_next(&s);
    if (val % 100u >= 15u)
        return len;

    /* Collect eligible (word_start, word_len) where word_len >= 3 and
     * we have at least one middle position (indices 1..len-2). */
    typedef struct {
        size_t start;
        size_t word_len;
    } word_t;
    word_t words[64];
    size_t word_count = 0;

    size_t i = 0;
    while (i < len && word_count < 64) {
        while (i < len && !is_word_char(buf[i]))
            i++;
        if (i >= len)
            break;
        size_t start = i;
        while (i < len && is_word_char(buf[i]))
            i++;
        size_t word_len = i - start;
        if (word_len >= 3 && word_len > 2) {
            words[word_count].start = start;
            words[word_count].word_len = word_len;
            word_count++;
        }
    }

    if (word_count == 0)
        return len;

    val = prng_next(&s);
    size_t word_idx = (size_t)(val % (uint32_t)word_count);
    size_t wstart = words[word_idx].start;
    size_t wlen = words[word_idx].word_len;
    size_t middle_count = wlen - 2;
    if (middle_count == 0)
        return len;

    val = prng_next(&s);
    size_t pos_in_word = 1u + (size_t)(val % (uint32_t)middle_count);
    size_t abs_pos = wstart + pos_in_word;

    val = prng_next(&s);
    uint32_t typo_type = val % 100u;

    char c = buf[abs_pos];
    char c_lower = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    if (c_lower < 'a' || c_lower > 'z')
        return len;

    if (typo_type < 40u) {
        /* Adjacent key swap */
        const char *neighbors = QWERTY_NEIGHBORS[(unsigned)(c_lower - 'a')];
        size_t ncount = 0;
        while (neighbors[ncount] != '\0')
            ncount++;
        if (ncount == 0)
            return len;
        val = prng_next(&s);
        char repl = neighbors[val % (uint32_t)ncount];
        if (c >= 'A' && c <= 'Z')
            repl = (char)(repl - 32);
        buf[abs_pos] = repl;
    } else if (typo_type < 70u) {
        /* Letter transposition: swap with next char (if letter) */
        if (abs_pos + 1 >= len || !is_word_char(buf[abs_pos + 1]))
            return len;
        char tmp = buf[abs_pos];
        buf[abs_pos] = buf[abs_pos + 1];
        buf[abs_pos + 1] = tmp;
    } else if (typo_type < 90u) {
        /* Dropped letter */
        memmove(buf + abs_pos, buf + abs_pos + 1, len - abs_pos - 1);
        buf[len - 1] = '\0';
        return len - 1;
    } else {
        /* Double letter: requires cap > len + 1 (len+1 chars + null) */
        if (cap <= len + 1)
            return len;
        memmove(buf + abs_pos + 1, buf + abs_pos, len - abs_pos);
        buf[abs_pos + 1] = buf[abs_pos];
        buf[len + 1] = '\0';
        return len + 1;
    }
    return len;
}

/* ── Two-phase "let me think" thinking response classifier ─────────────── */

typedef enum {
    SC_THINK_DECISION = 0,  /* advice/complex decision */
    SC_THINK_EMOTIONAL = 1, /* emotional support */
    SC_THINK_COMPLEX = 2,   /* philosophical/factual */
} sc_think_type_t;

static sc_think_type_t classify_think_type(const char *msg, size_t msg_len) {
    size_t excl = 0;
    size_t words = 0;
    for (size_t i = 0; i < msg_len; i++) {
        if (msg[i] == '!')
            excl++;
        if (msg[i] == ' ' || msg[i] == '\n')
            words++;
    }
    if (msg_len > 0)
        words++;

    /* Emotional: high exclamation density or short + intense */
    if (excl >= 2 || (excl >= 1 && msg_len < 60))
        return SC_THINK_EMOTIONAL;
    /* Complex: long message, often philosophical or multi-part */
    if (msg_len > 120 && words > 15)
        return SC_THINK_COMPLEX;
    /* Decision: medium-length question (advice-seeking) */
    return SC_THINK_DECISION;
}

bool sc_conversation_classify_thinking(const char *msg, size_t msg_len,
                                       const sc_channel_history_entry_t *entries,
                                       size_t entry_count, sc_thinking_response_t *out,
                                       uint32_t seed) {
    (void)entries;
    (void)entry_count;
    if (!msg || msg_len == 0 || !out)
        return false;

    memset(out, 0, sizeof(*out));

    /* Trigger: message characteristics */
    bool has_question = false;
    size_t words = 0;
    for (size_t i = 0; i < msg_len; i++) {
        if (msg[i] == '?')
            has_question = true;
        if (msg[i] == ' ' || msg[i] == '\n')
            words++;
    }
    if (msg_len > 0)
        words++;

    bool triggered = false;
    if (has_question && msg_len > 35 && words > 6)
        triggered = true;
    if (!triggered && msg_len > 80 && words > 10)
        triggered = true;

    if (!triggered)
        return false;

    sc_think_type_t t = classify_think_type(msg, msg_len);
    const char *fillers[4];
    size_t filler_count;
    switch (t) {
    case SC_THINK_EMOTIONAL:
        fillers[0] = "hmm";
        fillers[1] = "oh wow";
        fillers[2] = "oh";
        filler_count = 3;
        break;
    case SC_THINK_DECISION:
        fillers[0] = "ooh that's a tough one";
        fillers[1] = "let me think about that for a sec";
        fillers[2] = "hm good question";
        filler_count = 3;
        break;
    case SC_THINK_COMPLEX:
        fillers[0] = "that's a really good question";
        fillers[1] = "okay give me a sec";
        fillers[2] = "let me think about that";
        filler_count = 3;
        break;
    }

    uint32_t state = seed * 1103515245u + 12345u;
    uint32_t idx = (state >> 16) % (uint32_t)filler_count;
    const char *filler = fillers[idx];
    size_t flen = strlen(filler);
    if (flen >= sizeof(out->filler))
        flen = sizeof(out->filler) - 1;
    memcpy(out->filler, filler, flen);
    out->filler[flen] = '\0';
    out->filler_len = flen;

    uint32_t base = 30000;
    uint32_t extra = 0;
    if (msg_len > 150)
        extra = 30000;
    else if (msg_len > 100)
        extra = 15000;
    else if (msg_len > 80)
        extra = 10000;
    out->delay_ms = base + extra;
    if (out->delay_ms > 60000)
        out->delay_ms = 60000;

    return true;
}

/* ── Enhanced response action classification (message-property driven) ─── */

static size_t count_words(const char *msg, size_t msg_len) {
    size_t n = 0;
    bool in_word = false;
    for (size_t i = 0; i < msg_len; i++) {
        char c = msg[i];
        bool w = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 (c == '\'');
        if (w && !in_word) {
            in_word = true;
            n++;
        } else if (!w) {
            in_word = false;
        }
    }
    return n;
}

/* Tapbacks are system-generated (channel protocol), not natural language — minimal set. */
static bool is_tapback_reaction(const char *norm, size_t ni) {
    if (ni < 4)
        return false;
    if (ni <= 12 &&
        ((ni == 5 && memcmp(norm, "liked", 5) == 0) || (ni == 5 && memcmp(norm, "loved", 5) == 0) ||
         (ni == 7 && memcmp(norm, "laughed", 7) == 0) ||
         (ni == 10 && memcmp(norm, "emphasized", 10) == 0) ||
         (ni == 8 && memcmp(norm, "disliked", 8) == 0) ||
         (ni == 9 && memcmp(norm, "questioned", 9) == 0)))
        return true;
    /* "Loved an image", "Liked a message" etc. */
    if (ni >= 7 && (memcmp(norm, "lovedan", 7) == 0 || memcmp(norm, "likedan", 7) == 0))
        return true;
    if (ni >= 9 && memcmp(norm, "laughedat", 9) == 0)
        return true;
    return false;
}

sc_response_action_t sc_conversation_classify_response(const char *msg, size_t msg_len,
                                                       const sc_channel_history_entry_t *entries,
                                                       size_t entry_count,
                                                       uint32_t *delay_extra_ms) {
    if (!delay_extra_ms)
        return SC_RESPONSE_FULL;
    *delay_extra_ms = 0;

    if (!msg || msg_len == 0)
        return SC_RESPONSE_SKIP;

    /* Normalize for comparison */
    char norm[128];
    size_t ni = 0;
    for (size_t i = 0; i < msg_len && ni < sizeof(norm) - 1; i++) {
        char c = msg[i];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c != ' ' && c != '\n' && c != '\r')
            norm[ni++] = c;
    }
    norm[ni] = '\0';

    size_t word_count = count_words(msg, msg_len);
    bool has_question = (memchr(msg, '?', msg_len) != NULL);

    /* Skip: tapbacks (system vocabulary) */
    if (is_tapback_reaction(norm, ni))
        return SC_RESPONSE_SKIP;

    /* Skip: very short */
    if (msg_len <= 2)
        return SC_RESPONSE_SKIP;

    /* ok/k/okay: skip unless answering our question */
    if ((ni == 1 && norm[0] == 'k') || (ni == 2 && memcmp(norm, "ok", 2) == 0) ||
        (ni == 4 && memcmp(norm, "okay", 4) == 0)) {
        if (entries && entry_count > 0) {
            size_t checked = 0;
            for (size_t i = entry_count; i > 0 && checked < 3; i--) {
                if (entries[i - 1].from_me) {
                    checked++;
                    const char *t = entries[i - 1].text;
                    size_t tl = strlen(t);
                    for (size_t j = 0; j < tl; j++) {
                        if (t[j] == '?')
                            return SC_RESPONSE_SKIP;
                    }
                }
            }
        }
        return SC_RESPONSE_SKIP;
    }

    /* Farewells: short response, quick delay (before generic brief) */
    if (str_contains_ci(msg, msg_len, "goodnight") || str_contains_ci(msg, msg_len, "good night") ||
        str_contains_ci(msg, msg_len, "gotta go") || str_contains_ci(msg, msg_len, "ttyl") ||
        str_contains_ci(msg, msg_len, "heading out") ||
        str_contains_ci(msg, msg_len, "peace out") || str_contains_ci(msg, msg_len, "see ya") ||
        str_contains_ci(msg, msg_len, "catch you later") ||
        str_contains_ci(msg, msg_len, "i'm out")) {
        *delay_extra_ms = 1500;
        return SC_RESPONSE_BRIEF;
    }
    if (msg_len <= 10 &&
        (str_contains_ci(msg, msg_len, "bye") || str_contains_ci(msg, msg_len, "night") ||
         str_contains_ci(msg, msg_len, "later") || str_contains_ci(msg, msg_len, "gn") ||
         str_contains_ci(msg, msg_len, "cya"))) {
        *delay_extra_ms = 1500;
        return SC_RESPONSE_BRIEF;
    }

    /* Brief: short acknowledgment by properties (length, word count, no question) */
    if (msg_len < 15 && word_count <= 2 && !has_question)
        return SC_RESPONSE_BRIEF;

    /* Bad news: extended pause — show you're absorbing it */
    if (str_contains_ci(msg, msg_len, "passed away") ||
        str_contains_ci(msg, msg_len, "got fired") || str_contains_ci(msg, msg_len, "broke up") ||
        str_contains_ci(msg, msg_len, "bad news") ||
        str_contains_ci(msg, msg_len, "didn't make it") ||
        str_contains_ci(msg, msg_len, "got rejected") || str_contains_ci(msg, msg_len, "lost my")) {
        *delay_extra_ms = 12000;
        return SC_RESPONSE_DELAY;
    }

    /* Good news: short pause then celebrate */
    if (str_contains_ci(msg, msg_len, "got the job") || str_contains_ci(msg, msg_len, "got in") ||
        str_contains_ci(msg, msg_len, "i passed") || str_contains_ci(msg, msg_len, "got engaged") ||
        str_contains_ci(msg, msg_len, "got promoted") ||
        str_contains_ci(msg, msg_len, "good news") || str_contains_ci(msg, msg_len, "i did it") ||
        str_contains_ci(msg, msg_len, "we did it")) {
        *delay_extra_ms = 3000;
        return SC_RESPONSE_DELAY;
    }

    /* Vulnerability: deliberate pause */
    if (str_contains_ci(msg, msg_len, "i need to tell you") ||
        str_contains_ci(msg, msg_len, "can i be honest") ||
        str_contains_ci(msg, msg_len, "don't judge me") ||
        str_contains_ci(msg, msg_len, "this is hard to say") ||
        str_contains_ci(msg, msg_len, "i never told")) {
        *delay_extra_ms = 8000;
        return SC_RESPONSE_DELAY;
    }

    /* Emotional/heavy messages: full response but delayed (showing you're thinking) */
    static const char *emotional[] = {
        "miss",    "love",     "hurt",   "stress",    "depress",   "lonely",     "scared",
        "worried", "sorry",    "afraid", "giving up", "feel like", "don't know", "can't",
        "help me", "need you", "cry",    "sad",       NULL,
    };
    for (int i = 0; emotional[i]; i++) {
        size_t elen = strlen(emotional[i]);
        for (size_t j = 0; j + elen <= msg_len; j++) {
            bool match = true;
            for (size_t k = 0; k < elen; k++) {
                char a = msg[j + k];
                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (a != emotional[i][k]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                *delay_extra_ms = 8000;
                return SC_RESPONSE_DELAY;
            }
        }
    }

    /* Statement without question: consider if the conversation is winding down.
     * If their last 3 messages were getting shorter and this one has no question,
     * they may not expect a response. Respond BRIEF. */
    if (!has_question && entries && entry_count >= 3) {
        size_t their_recent = 0;
        size_t getting_shorter = 0;
        size_t prev_len = 999;
        for (size_t i = entry_count; i > 0 && their_recent < 3; i--) {
            if (!entries[i - 1].from_me) {
                size_t tl = strlen(entries[i - 1].text);
                if (tl < prev_len && prev_len != 999)
                    getting_shorter++;
                prev_len = tl;
                their_recent++;
            }
        }
        if (getting_shorter >= 2 && msg_len < 30) {
            return SC_RESPONSE_BRIEF;
        }
    }

    /* Question: normal response, moderate thinking delay */
    if (has_question) {
        *delay_extra_ms = 2000;
        return SC_RESPONSE_FULL;
    }

    return SC_RESPONSE_FULL;
}

/* ── URL extraction ──────────────────────────────────────────────────── */

/* Utility for future use. Not currently wired into production; link-sharing
 * logic uses sc_conversation_should_share_link with pattern matching instead. */
size_t sc_conversation_extract_urls(const char *text, size_t text_len, sc_url_extract_t *urls,
                                    size_t max_urls) {
    if (!text || !urls || max_urls == 0)
        return 0;

    size_t count = 0;
    const char *p = text;
    const char *end = text + text_len;

    while (p < end && count < max_urls) {
        const char *start = NULL;
        size_t url_len = 0;

        if (p + 8 <= end && strncmp(p, "https://", 8) == 0) {
            start = p;
            p += 8;
            while (p < end) {
                char c = *p;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' || c == ')' ||
                    c == ']' || c == '"' || c == '\'')
                    break;
                p++;
            }
            url_len = (size_t)(p - start);
        } else if (p + 7 <= end && strncmp(p, "http://", 7) == 0) {
            start = p;
            p += 7;
            while (p < end) {
                char c = *p;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' || c == ')' ||
                    c == ']' || c == '"' || c == '\'')
                    break;
                p++;
            }
            url_len = (size_t)(p - start);
        } else {
            p++;
        }

        if (start && url_len > 0 && url_len <= 2048) {
            urls[count].start = start;
            urls[count].len = url_len;
            count++;
        }
    }
    return count;
}

/* ── Link-sharing detection ───────────────────────────────────────────── */

static bool msg_contains_recommendation_pattern(const char *msg, size_t msg_len) {
    static const char *patterns[] = {
        "check this out", "have you seen", "you should try", "recommend",      "link",
        "article",        "look at this",  "here's a link",  "here is a link", NULL,
    };
    for (int i = 0; patterns[i]; i++) {
        if (str_contains_ci(msg, msg_len, patterns[i]))
            return true;
    }
    return false;
}

bool sc_conversation_should_share_link(const char *msg, size_t msg_len,
                                       const sc_channel_history_entry_t *entries,
                                       size_t entry_count) {
    if (!msg || msg_len == 0)
        return false;

    if (msg_contains_recommendation_pattern(msg, msg_len))
        return true;

    /* Check if our previous response (last from_me) mentions wanting to share something */
    if (entries && entry_count > 0) {
        for (size_t i = entry_count; i > 0; i--) {
            if (entries[i - 1].from_me) {
                const char *prev = entries[i - 1].text;
                size_t prev_len = strlen(prev);
                if (str_contains_ci(prev, prev_len, "share") ||
                    str_contains_ci(prev, prev_len, "link") ||
                    str_contains_ci(prev, prev_len, "check out") ||
                    str_contains_ci(prev, prev_len, "here's") ||
                    str_contains_ci(prev, prev_len, "here is"))
                    return true;
                break;
            }
        }
    }
    return false;
}

/* ── Attachment context for prompts ──────────────────────────────────── */

static bool is_attachment_placeholder(const char *text, size_t len) {
    if (!text || len < 10)
        return false;
    if (strstr(text, "[image or attachment]"))
        return true;
    if (strstr(text, "[Photo shared]"))
        return true;
    if (strstr(text, "[Video shared]"))
        return true;
    if (strstr(text, "[Audio message]"))
        return true;
    if (strstr(text, "[Attachment shared]"))
        return true;
    if (strstr(text, "[Document:"))
        return true;
    return false;
}

char *sc_conversation_attachment_context(sc_allocator_t *alloc,
                                         const sc_channel_history_entry_t *entries, size_t count,
                                         size_t *out_len) {
    if (!alloc || !out_len || !entries || count == 0)
        return NULL;
    *out_len = 0;

    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        if (is_attachment_placeholder(t, tl)) {
            found = true;
            break;
        }
    }
    if (!found)
        return NULL;

    const char *ctx =
        "The user shared a photo/attachment. Acknowledge it naturally — "
        "\"love that!\", \"that looks great\", etc. Don't say \"I can see the image\" "
        "if you can't actually analyze it.";
    size_t ctx_len = strlen(ctx);
    char *result = sc_strndup(alloc, ctx, ctx_len);
    if (result)
        *out_len = ctx_len;
    return result;
}

/* ── Anti-repetition detection ────────────────────────────────────────── */

size_t sc_conversation_detect_repetition(const sc_channel_history_entry_t *entries, size_t count,
                                         char *buf, size_t cap) {
    if (!entries || count < 4 || !buf || cap < 64)
        return 0;

    /* Collect last N "from_me" messages */
    const char *my_msgs[8];
    size_t my_count = 0;
    for (size_t i = count; i > 0 && my_count < 8; i--) {
        if (entries[i - 1].from_me) {
            my_msgs[my_count++] = entries[i - 1].text;
        }
    }
    if (my_count < 3)
        return 0;

    size_t pos = 0;
    int w;
    bool found = false;

    /* Detect repeated openers (first word of each message) */
    char openers[8][16];
    for (size_t i = 0; i < my_count; i++) {
        size_t j = 0;
        const char *m = my_msgs[i];
        while (m[j] && m[j] != ' ' && j < 15)
            j++;
        if (j > 0 && j < 15) {
            for (size_t k = 0; k < j; k++) {
                char c = m[k];
                if (c >= 'A' && c <= 'Z')
                    c += 32;
                openers[i][k] = c;
            }
            openers[i][j] = '\0';
        } else {
            openers[i][0] = '\0';
        }
    }

    /* Check if same opener used 3+ times in last 5 messages */
    for (size_t i = 0; i < my_count && i < 5; i++) {
        if (openers[i][0] == '\0')
            continue;
        size_t matches = 0;
        for (size_t j = 0; j < my_count && j < 5; j++) {
            if (strcmp(openers[i], openers[j]) == 0)
                matches++;
        }
        if (matches >= 3) {
            if (!found) {
                w = snprintf(buf + pos, cap - pos, "\n--- Anti-repetition ---\n");
                POS_ADVANCE(w, pos, cap);
                found = true;
            }
            w = snprintf(buf + pos, cap - pos,
                         "WARNING: You've started %zu of your last messages with '%s'. "
                         "Vary your openers. Start differently this time.\n",
                         matches, openers[i]);
            POS_ADVANCE(w, pos, cap);
            break;
        }
    }

    /* Check if always ending with a question */
    size_t questions = 0;
    size_t check = my_count < 5 ? my_count : 5;
    for (size_t i = 0; i < check; i++) {
        const char *m = my_msgs[i];
        size_t ml = strlen(m);
        if (ml > 0 && m[ml - 1] == '?')
            questions++;
    }
    if (questions >= 3) {
        if (!found) {
            w = snprintf(buf + pos, cap - pos, "\n--- Anti-repetition ---\n");
            POS_ADVANCE(w, pos, cap);
            found = true;
        }
        w = snprintf(buf + pos, cap - pos,
                     "WARNING: You've ended %zu of your last %zu messages with a question. "
                     "Not every message needs a follow-up question. "
                     "Make a statement, react, or just let it sit.\n",
                     questions, check);
        POS_ADVANCE(w, pos, cap);
    }

    /* Check if always using "haha"/"lol" as filler */
    size_t laughs = 0;
    for (size_t i = 0; i < check; i++) {
        if (str_contains_ci(my_msgs[i], strlen(my_msgs[i]), "haha") ||
            str_contains_ci(my_msgs[i], strlen(my_msgs[i]), "lol"))
            laughs++;
    }
    if (laughs >= 3) {
        if (!found) {
            w = snprintf(buf + pos, cap - pos, "\n--- Anti-repetition ---\n");
            POS_ADVANCE(w, pos, cap);
            found = true;
        }
        w = snprintf(buf + pos, cap - pos,
                     "WARNING: You've used 'haha'/'lol' in %zu of your last %zu messages. "
                     "Drop the nervous laughter. Not everything needs softening.\n",
                     laughs, check);
        POS_ADVANCE(w, pos, cap);
    }

    if (found) {
        w = snprintf(buf + pos, cap - pos, "--- End anti-repetition ---\n");
        POS_ADVANCE(w, pos, cap);
    }

    return pos;
}

/* ── Relationship-tier calibration ────────────────────────────────────── */

size_t sc_conversation_calibrate_relationship(const char *relationship_stage,
                                              const char *warmth_level,
                                              const char *vulnerability_level, char *buf,
                                              size_t cap) {
    if (!buf || cap < 64)
        return 0;

    size_t pos = 0;
    int w = snprintf(buf, cap, "\n--- Relationship context ---\n");
    POS_ADVANCE(w, pos, cap);

    /* Relationship stage → engagement depth */
    if (relationship_stage) {
        if (str_contains_ci(relationship_stage, strlen(relationship_stage), "close") ||
            str_contains_ci(relationship_stage, strlen(relationship_stage), "best") ||
            str_contains_ci(relationship_stage, strlen(relationship_stage), "partner") ||
            str_contains_ci(relationship_stage, strlen(relationship_stage), "intimate")) {
            w = snprintf(buf + pos, cap - pos,
                         "RELATIONSHIP: Close. You can be fully yourself — inside jokes, "
                         "mild roasting, deep honesty, comfortable silence. "
                         "Don't over-explain or be overly polite.\n");
        } else if (str_contains_ci(relationship_stage, strlen(relationship_stage), "friend")) {
            w = snprintf(buf + pos, cap - pos,
                         "RELATIONSHIP: Friend. Warm and genuine but with some boundaries. "
                         "Banter is fine. Personal topics ok if they bring them up first.\n");
        } else if (str_contains_ci(relationship_stage, strlen(relationship_stage), "acquaint") ||
                   str_contains_ci(relationship_stage, strlen(relationship_stage), "new")) {
            w = snprintf(buf + pos, cap - pos,
                         "RELATIONSHIP: Acquaintance/new. Keep it lighter. "
                         "Don't assume familiarity. Match their formality level. "
                         "No inside jokes. No unsolicited deep topics.\n");
        } else if (str_contains_ci(relationship_stage, strlen(relationship_stage),
                                   "professional") ||
                   str_contains_ci(relationship_stage, strlen(relationship_stage), "work")) {
            w = snprintf(buf + pos, cap - pos,
                         "RELATIONSHIP: Professional. Stay on topic. "
                         "Friendly but not casual. No slang, minimal emoji.\n");
        } else {
            w = snprintf(buf + pos, cap - pos, "RELATIONSHIP: %s. Calibrate formality to match.\n",
                         relationship_stage);
        }
        POS_ADVANCE(w, pos, cap);
    }

    /* Warmth level → emotional expression */
    if (warmth_level) {
        int ww = 0;
        if (str_contains_ci(warmth_level, strlen(warmth_level), "high")) {
            ww = snprintf(buf + pos, cap - pos,
                          "WARMTH: High. Affectionate language is natural with this person.\n");
        } else if (str_contains_ci(warmth_level, strlen(warmth_level), "low")) {
            ww = snprintf(buf + pos, cap - pos,
                          "WARMTH: Low. Keep it cool and understated. No gushing.\n");
        }
        POS_ADVANCE(ww, pos, cap);
    }

    /* Vulnerability level → depth permission */
    if (vulnerability_level) {
        int vw = 0;
        if (str_contains_ci(vulnerability_level, strlen(vulnerability_level), "high") ||
            str_contains_ci(vulnerability_level, strlen(vulnerability_level), "open")) {
            vw = snprintf(buf + pos, cap - pos,
                          "VULNERABILITY: Open. You can share real feelings and be honest "
                          "about struggles. This person can handle depth.\n");
        } else if (str_contains_ci(vulnerability_level, strlen(vulnerability_level), "low") ||
                   str_contains_ci(vulnerability_level, strlen(vulnerability_level), "guarded")) {
            vw = snprintf(buf + pos, cap - pos,
                          "VULNERABILITY: Guarded. Keep emotional sharing surface-level. "
                          "Don't dump feelings. Stay light.\n");
        }
        POS_ADVANCE(vw, pos, cap);
    }

    w = snprintf(buf + pos, cap - pos, "--- End relationship ---\n");
    POS_ADVANCE(w, pos, cap);

    return pos;
}

/* ── Group chat classifier ────────────────────────────────────────────── */

sc_group_response_t sc_conversation_classify_group(const char *msg, size_t msg_len,
                                                   const char *bot_name, size_t bot_name_len,
                                                   const sc_channel_history_entry_t *entries,
                                                   size_t count) {
    if (!msg || msg_len == 0)
        return SC_GROUP_SKIP;

    /* Always respond if directly addressed */
    if (bot_name && bot_name_len > 0 && str_contains_ci(msg, msg_len, bot_name))
        return SC_GROUP_RESPOND;

    /* Always respond to direct questions (contains "?" and is short) */
    bool has_question = false;
    for (size_t i = 0; i < msg_len; i++) {
        if (msg[i] == '?') {
            has_question = true;
            break;
        }
    }
    if (has_question && msg_len < 100)
        return SC_GROUP_RESPOND;

    /* Skip tapbacks and reactions */
    if (msg_len <= 3)
        return SC_GROUP_SKIP;

    /* Skip if we responded to the last 2 messages already (don't dominate) */
    if (entries && count >= 3) {
        size_t consecutive_mine = 0;
        for (size_t i = count; i > 0 && consecutive_mine < 3; i--) {
            if (entries[i - 1].from_me)
                consecutive_mine++;
            else
                break;
        }
        if (consecutive_mine >= 2)
            return SC_GROUP_SKIP;
    }

    /* Count how much of the recent conversation we've participated in.
     * If we've responded to >40% of the last 10 messages, dial back. */
    if (entries && count >= 6) {
        size_t window = count < 10 ? count : 10;
        size_t my_msgs = 0;
        for (size_t i = count - window; i < count; i++) {
            if (entries[i].from_me)
                my_msgs++;
        }
        if (my_msgs * 100 / window > 40)
            return SC_GROUP_SKIP;
    }

    /* Emotional content or someone asking for help → respond */
    static const char *engage_words[] = {
        "help", "anyone", "thoughts?", "what do you", "need", "advice", "opinion", NULL,
    };
    for (int i = 0; engage_words[i]; i++) {
        if (str_contains_ci(msg, msg_len, engage_words[i]))
            return SC_GROUP_RESPOND;
    }

    /* Short message with no clear prompt → skip */
    if (msg_len < 30 && !has_question)
        return SC_GROUP_SKIP;

    /* Default: brief acknowledgment for medium messages, skip for long ones
     * (long messages in group chats are usually directed at specific people) */
    if (msg_len > 100)
        return SC_GROUP_SKIP;

    return SC_GROUP_BRIEF;
}

/* ── Reaction classifier ───────────────────────────────────────────────── */

static uint32_t reaction_prng_next(uint32_t *s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16u) & 0x7fffu;
}

sc_reaction_type_t sc_conversation_classify_reaction(const char *msg, size_t msg_len, bool from_me,
                                                     const sc_channel_history_entry_t *entries,
                                                     size_t entry_count, uint32_t seed) {
    (void)entries;
    (void)entry_count;

    if (!msg || msg_len == 0)
        return SC_REACTION_NONE;

    /* Only react to messages from others, not our own */
    if (from_me)
        return SC_REACTION_NONE;

    uint32_t s = seed;

    /* Photos/media placeholders: 50% chance of heart */
    if (str_contains_ci(msg, msg_len, "[image") || str_contains_ci(msg, msg_len, "[attachment")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 50u)
            return SC_REACTION_HEART;
        return SC_REACTION_NONE;
    }

    /* Funny messages → HAHA */
    if (str_contains_ci(msg, msg_len, "lol") || str_contains_ci(msg, msg_len, "lmao") ||
        str_contains_ci(msg, msg_len, "haha") || str_contains_ci(msg, msg_len, "hahaha") ||
        str_contains_ci(msg, msg_len, "😂") || str_contains_ci(msg, msg_len, "hilarious") ||
        str_contains_ci(msg, msg_len, "that's funny") ||
        str_contains_ci(msg, msg_len, "so funny")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 30u)
            return SC_REACTION_HAHA;
        return SC_REACTION_NONE;
    }

    /* Short message with exclamations (e.g. "omg!!", "yes!!!") → HAHA or EMPHASIS */
    if (msg_len <= 20) {
        bool has_excl = false;
        for (size_t i = 0; i < msg_len; i++) {
            if (msg[i] == '!') {
                has_excl = true;
                break;
            }
        }
        if (has_excl) {
            uint32_t roll = reaction_prng_next(&s) % 100u;
            if (roll < 30u)
                return SC_REACTION_HAHA;
        }
    }

    /* Loving/sweet messages → HEART */
    if (str_contains_ci(msg, msg_len, "love you") || str_contains_ci(msg, msg_len, "miss you") ||
        str_contains_ci(msg, msg_len, "❤") || str_contains_ci(msg, msg_len, "💕") ||
        str_contains_ci(msg, msg_len, "you're amazing") ||
        str_contains_ci(msg, msg_len, "you're the best") ||
        str_contains_ci(msg, msg_len, "proud of you") ||
        str_contains_ci(msg, msg_len, "so sweet")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 30u)
            return SC_REACTION_HEART;
        return SC_REACTION_NONE;
    }

    /* Agreement/affirmation → THUMBS_UP */
    if (str_contains_ci(msg, msg_len, "absolutely") || str_contains_ci(msg, msg_len, "exactly") ||
        str_contains_ci(msg, msg_len, "yes!") || str_contains_ci(msg, msg_len, "👍") ||
        str_contains_ci(msg, msg_len, "for sure") || str_contains_ci(msg, msg_len, "definitely")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 30u)
            return SC_REACTION_THUMBS_UP;
        return SC_REACTION_NONE;
    }

    /* Impressive/exciting news → EMPHASIS */
    if (str_contains_ci(msg, msg_len, "got the job") || str_contains_ci(msg, msg_len, "i did it") ||
        str_contains_ci(msg, msg_len, "we won") || str_contains_ci(msg, msg_len, "we did it") ||
        str_contains_ci(msg, msg_len, "got in") || str_contains_ci(msg, msg_len, "i passed") ||
        str_contains_ci(msg, msg_len, "got engaged") ||
        str_contains_ci(msg, msg_len, "got promoted")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 30u)
            return SC_REACTION_EMPHASIS;
        return SC_REACTION_NONE;
    }

    /* Messages that need a real text response → NONE */
    if (str_contains_ci(msg, msg_len, "what time") || str_contains_ci(msg, msg_len, "where") ||
        str_contains_ci(msg, msg_len, "how do") || str_contains_ci(msg, msg_len, "can you") ||
        str_contains_ci(msg, msg_len, "could you") || str_contains_ci(msg, msg_len, "why") ||
        str_contains_ci(msg, msg_len, "when") || str_contains_ci(msg, msg_len, "?"))
        return SC_REACTION_NONE;

    return SC_REACTION_NONE;
}
