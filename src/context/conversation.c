#include "seaclaw/context/conversation.h"
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

    /* ── Emotional / situational analysis ────────────────────────────── */
    {
        bool they_seem_frustrated = false;
        bool they_seem_excited = false;
        bool they_seem_sad = false;
        bool open_question = false;
        bool they_sent_link = false;
        bool logistics_thread = false;

        for (size_t i = 0; i < count; i++) {
            const char *t = entries[i].text;
            size_t tl = strlen(t);
            if (!entries[i].from_me) {
                for (size_t j = 0; j < tl; j++) {
                    if (t[j] == '?')
                        open_question = true;
                    if (t[j] == '!' && tl > 3)
                        they_seem_excited = true;
                }
                for (size_t j = 0; j + 3 < tl; j++) {
                    char lo[5];
                    for (int k = 0; k < 4 && j + (size_t)k < tl; k++) {
                        lo[k] = t[j + k];
                        if (lo[k] >= 'A' && lo[k] <= 'Z')
                            lo[k] += 32;
                    }
                    lo[4] = 0;
                    if (strncmp(lo, "damn", 4) == 0 || strncmp(lo, "ugh", 3) == 0)
                        they_seem_frustrated = true;
                    if (strncmp(lo, "sad", 3) == 0 || strncmp(lo, "cry", 3) == 0)
                        they_seem_sad = true;
                }
                for (size_t j = 0; j + 4 < tl; j++) {
                    if (memcmp(t + j, "http", 4) == 0)
                        they_sent_link = true;
                }
            }
            for (size_t j = 0; j + 5 < tl; j++) {
                char lo[6];
                for (int k = 0; k < 5 && j + (size_t)k < tl; k++) {
                    lo[k] = t[j + k];
                    if (lo[k] >= 'A' && lo[k] <= 'Z')
                        lo[k] += 32;
                }
                lo[5] = 0;
                if (strcmp(lo, "fligh") == 0 || strcmp(lo, "airpo") == 0 ||
                    strcmp(lo, "leavi") == 0 || strcmp(lo, "booke") == 0 ||
                    strcmp(lo, "arriv") == 0)
                    logistics_thread = true;
            }
        }

        w = snprintf(buf + pos, CTX_BUF_CAP - pos, "--- Conversation awareness ---\n");
        POS_ADVANCE(w, pos, CTX_BUF_CAP);

        /* Time-of-day context triggers */
        {
            time_t now = time(NULL);
            struct tm *lt = localtime(&now);
            if (lt) {
                int hour = lt->tm_hour;
                int wday = lt->tm_wday;
                if (hour >= 0 && hour < 6) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "TIME: Very late at night. Be gentler, softer, no pressure. "
                                 "Short responses.\n");
                } else if (hour >= 6 && hour < 9) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "TIME: Early morning. Keep it light and warm.\n");
                } else if (hour >= 21) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "TIME: Evening/night. Relaxed energy, shorter texts.\n");
                } else {
                    w = 0;
                }
                POS_ADVANCE(w, pos, CTX_BUF_CAP);

                if (wday == 1) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "DAY: Monday. Be a bit more supportive.\n");
                } else if (wday == 5) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "DAY: Friday. Weekend energy, lighter and upbeat.\n");
                } else if (wday == 0 && hour >= 17) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos, "DAY: Sunday evening. Be gentle.\n");
                } else if (wday == 0 || wday == 6) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos, "DAY: Weekend. Relaxed vibe.\n");
                } else {
                    w = 0;
                }
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
        }

        if (they_seem_frustrated) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "They seem frustrated. Be calm, acknowledge it.\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }
        if (they_seem_excited) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "They seem excited. Be genuinely happy for them.\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }
        if (they_seem_sad) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "They seem sad or down. Be present and gentle.\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }
        if (open_question) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "They asked a question. Make sure you answer it.\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }
        if (they_sent_link) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "They shared a link. Acknowledge or comment on it.\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }
        if (logistics_thread) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "Active logistics/travel thread. Stay on topic.\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }

        /* Detected state analysis */
        {
            bool seems_rushed = false;
            bool seems_tired = false;
            size_t short_msg_count = 0;
            size_t their_recent = 0;
            size_t start = count > 5 ? count - 5 : 0;
            for (size_t i = start; i < count; i++) {
                if (!entries[i].from_me) {
                    their_recent++;
                    if (strlen(entries[i].text) < 15)
                        short_msg_count++;
                }
            }
            if (their_recent >= 3 && short_msg_count >= 2)
                seems_rushed = true;
            {
                time_t now = time(NULL);
                struct tm *lt = localtime(&now);
                if (lt && (lt->tm_hour >= 23 || lt->tm_hour < 5)) {
                    if (count > 0 && !entries[count - 1].from_me &&
                        strlen(entries[count - 1].text) < 30)
                        seems_tired = true;
                }
            }
            if (seems_rushed) {
                w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                             "STATE: They seem rushed. Get to the point.\n");
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
            if (seems_tired) {
                w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                             "STATE: They might be tired. Be brief and gentle.\n");
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
        }

        /* Verbosity mirroring */
        {
            size_t their_total_chars = 0;
            size_t their_msg_count = 0;
            for (size_t i = 0; i < count; i++) {
                if (!entries[i].from_me && strlen(entries[i].text) > 2) {
                    their_total_chars += strlen(entries[i].text);
                    their_msg_count++;
                }
            }
            if (their_msg_count > 0) {
                size_t avg_len = their_total_chars / their_msg_count;
                if (avg_len < 30) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "RESPONSE LENGTH: They text very briefly (avg %zu chars). "
                                 "Match — 1 sentence max, under 50 chars.\n",
                                 avg_len);
                } else if (avg_len < 80) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "RESPONSE LENGTH: Short messages (avg %zu chars). "
                                 "1-2 short sentences, under 150 chars.\n",
                                 avg_len);
                } else {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "RESPONSE LENGTH: Longer messages (avg %zu chars). "
                                 "2-3 sentences max, under 250 chars.\n",
                                 avg_len);
                }
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
        }

        /* Conversation phase detection */
        {
            size_t exchanges = 0;
            bool last_was_me = false;
            for (size_t i = 0; i < count; i++) {
                if (i == 0 || entries[i].from_me != last_was_me)
                    exchanges++;
                last_was_me = entries[i].from_me;
            }
            if (exchanges <= 2) {
                w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                             "PHASE: Opening. Keep it light and warm. Short greeting.\n");
            } else if (exchanges <= 8) {
                w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                             "PHASE: Engaged. Build on what they said. Match depth.\n");
            } else {
                w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                             "PHASE: Deep/winding. Been texting a while. Ok to be briefer.\n");
            }
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }

        /* Real-time noticing (VoiceAI realtime-noticing) */
        {
            /* Detect energy drop: their messages getting shorter over time */
            if (count >= 6) {
                size_t first_half_len = 0, first_half_n = 0;
                size_t second_half_len = 0, second_half_n = 0;
                size_t mid = count / 2;
                for (size_t i = 0; i < count; i++) {
                    if (!entries[i].from_me) {
                        size_t tl = strlen(entries[i].text);
                        if (i < mid) {
                            first_half_len += tl;
                            first_half_n++;
                        } else {
                            second_half_len += tl;
                            second_half_n++;
                        }
                    }
                }
                if (first_half_n > 0 && second_half_n > 0) {
                    size_t avg1 = first_half_len / first_half_n;
                    size_t avg2 = second_half_len / second_half_n;
                    if (avg1 > 40 && avg2 < avg1 / 2) {
                        w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                     "NOTICE: Their messages are getting shorter "
                                     "(energy dropping). Be gentler, check in.\n");
                        POS_ADVANCE(w, pos, CTX_BUF_CAP);
                    }
                }
            }

            /* Detect repeated theme: same words appearing 3+ times across their messages */
            {
                typedef struct {
                    char word[32];
                    int hits;
                } word_freq_t;
                word_freq_t freq[16];
                size_t freq_n = 0;
                for (size_t i = 0; i < count; i++) {
                    if (entries[i].from_me)
                        continue;
                    const char *t = entries[i].text;
                    size_t tl = strlen(t);
                    size_t wi = 0;
                    while (wi < tl) {
                        while (wi < tl && (t[wi] == ' ' || t[wi] == '\n'))
                            wi++;
                        size_t ws = wi;
                        while (wi < tl && t[wi] != ' ' && t[wi] != '\n')
                            wi++;
                        size_t wlen = wi - ws;
                        if (wlen < 4 || wlen > 30)
                            continue;
                        char word[32];
                        for (size_t k = 0; k < wlen && k < 31; k++) {
                            word[k] = t[ws + k];
                            if (word[k] >= 'A' && word[k] <= 'Z')
                                word[k] += 32;
                        }
                        word[wlen < 31 ? wlen : 31] = '\0';
                        /* Skip common words */
                        if (strcmp(word, "the") == 0 || strcmp(word, "and") == 0 ||
                            strcmp(word, "that") == 0 || strcmp(word, "this") == 0 ||
                            strcmp(word, "with") == 0 || strcmp(word, "have") == 0 ||
                            strcmp(word, "just") == 0 || strcmp(word, "like") == 0 ||
                            strcmp(word, "know") == 0 || strcmp(word, "what") == 0 ||
                            strcmp(word, "from") == 0 || strcmp(word, "about") == 0)
                            continue;
                        bool found = false;
                        for (size_t f = 0; f < freq_n; f++) {
                            if (strcmp(freq[f].word, word) == 0) {
                                freq[f].hits++;
                                found = true;
                                break;
                            }
                        }
                        if (!found && freq_n < 16) {
                            memcpy(freq[freq_n].word, word, strlen(word) + 1);
                            freq[freq_n].hits = 1;
                            freq_n++;
                        }
                    }
                }
                for (size_t f = 0; f < freq_n; f++) {
                    if (freq[f].hits >= 3) {
                        w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                     "NOTICE: They keep mentioning '%s' "
                                     "(%d times). This matters to them.\n",
                                     freq[f].word, freq[f].hits);
                        POS_ADVANCE(w, pos, CTX_BUF_CAP);
                        break;
                    }
                }
            }

            /* Detect topic deflection: they asked you something, you answered,
             * they immediately changed topic without acknowledging */
            if (count >= 3) {
                for (size_t i = 2; i < count; i++) {
                    if (i >= 2 && !entries[i - 2].from_me && entries[i - 1].from_me &&
                        !entries[i].from_me) {
                        const char *q = entries[i - 2].text;
                        const char *r = entries[i].text;
                        bool had_question = false;
                        for (size_t j = 0; j < strlen(q); j++)
                            if (q[j] == '?')
                                had_question = true;
                        if (had_question && strlen(r) > 10) {
                            bool shares_words = false;
                            /* Quick check: does the follow-up share any significant word with the
                             * question? */
                            for (size_t j = 0; j + 4 < strlen(q); j++) {
                                char chunk[6];
                                for (int k = 0; k < 5; k++) {
                                    chunk[k] = q[j + k];
                                    if (chunk[k] >= 'A' && chunk[k] <= 'Z')
                                        chunk[k] += 32;
                                }
                                chunk[5] = '\0';
                                /* Check if this 5-char chunk appears in the follow-up */
                                for (size_t k = 0; k + 4 < strlen(r); k++) {
                                    char rc[6];
                                    for (int l = 0; l < 5; l++) {
                                        rc[l] = r[k + l];
                                        if (rc[l] >= 'A' && rc[l] <= 'Z')
                                            rc[l] += 32;
                                    }
                                    rc[5] = '\0';
                                    if (strcmp(chunk, rc) == 0) {
                                        shares_words = true;
                                        break;
                                    }
                                }
                                if (shares_words)
                                    break;
                            }
                            if (!shares_words) {
                                w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                             "NOTICE: They may have deflected from a topic. "
                                             "Don't force it — follow their lead.\n");
                                POS_ADVANCE(w, pos, CTX_BUF_CAP);
                                break;
                            }
                        }
                    }
                }
            }
        }

        /* Narrative arc tracking */
        {
            sc_narrative_phase_t arc = sc_conversation_detect_narrative(entries, count);
            const char *arc_labels[] = {
                "OPENING — Keep light, warm greeting. Don't go deep yet.",
                "BUILDING — They're engaged. Match their energy, build rapport.",
                "APPROACHING CLIMAX — Conversation is intensifying. "
                "Be present, listen more than you talk.",
                "PEAK — Emotional peak. Hold space. Validate. Don't fix. "
                "This is where real connection happens.",
                "RELEASE — Intensity is easing. Lighter touches. "
                "Maybe humor if appropriate.",
                "CLOSING — They're wrapping up. Keep it brief, warm sign-off. "
                "Don't start new topics.",
            };
            w = snprintf(buf + pos, CTX_BUF_CAP - pos, "ARC: %s\n", arc_labels[arc]);
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }

        /* Engagement scoring */
        {
            sc_engagement_level_t eng = sc_conversation_detect_engagement(entries, count);
            if (eng == SC_ENGAGEMENT_LOW) {
                w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                             "ENGAGEMENT: Low. Try changing topic, share something "
                             "surprising, or ask a genuine question.\n");
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            } else if (eng == SC_ENGAGEMENT_DISTRACTED) {
                w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                             "ENGAGEMENT: Distracted. They may be busy. "
                             "Keep responses ultra-short or let them re-engage.\n");
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            } else if (eng == SC_ENGAGEMENT_HIGH) {
                w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                             "ENGAGEMENT: High. They're invested. Match depth.\n");
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
        }

        /* Emotional state */
        {
            sc_emotional_state_t emo = sc_conversation_detect_emotion(entries, count);
            if (emo.intensity > 0.3f) {
                w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                             "EMOTION: They seem %s (intensity: %.1f). ", emo.dominant_emotion,
                             (double)emo.intensity);
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
                if (emo.concerning) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "This is concerning. Be gentle and present. "
                                 "Check in directly.\n");
                } else if (emo.valence < -0.3f) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "Acknowledge their feelings before anything else.\n");
                } else if (emo.valence > 0.3f) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos, "Share in their positive energy.\n");
                } else {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos, "\n");
                }
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
        }

        w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                     "\nUse this context naturally. Reference specific details they mentioned. "
                     "Do NOT summarize or acknowledge this context aloud.\n");
        POS_ADVANCE(w, pos, CTX_BUF_CAP);
    }

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
            size_t cal_len = sc_conversation_calibrate_length(last_their_msg, last_their_len,
                                                              entries, count, buf + pos,
                                                              CTX_BUF_CAP - pos);
            pos += cal_len;
        }
    }

    buf[pos] = '\0';
    *out_len = pos;
    return buf;
}

/* ── Conversation Quality Evaluator ─────────────────────────────────── */

sc_quality_score_t sc_conversation_evaluate_quality(const char *response, size_t response_len,
                                                    const sc_channel_history_entry_t *entries,
                                                    size_t count, uint32_t max_chars) {
    sc_quality_score_t score = {0, 0, 0, 0, 0, false};
    if (!response || response_len == 0)
        return score;

    /* Brevity (0-25): shorter is better for text channels */
    if (max_chars > 0) {
        if (response_len <= max_chars / 2)
            score.brevity = 25;
        else if (response_len <= max_chars)
            score.brevity = 20;
        else if (response_len <= max_chars * 2)
            score.brevity = 10;
        else
            score.brevity = 0;
    } else {
        score.brevity = response_len < 200 ? 25 : response_len < 500 ? 15 : 5;
    }

    /* Validation (0-25): does the response reflect/acknowledge their feelings? */
    {
        bool has_validation = false;
        const char *validators[] = {"that makes sense",
                                    "i get it",
                                    "i understand",
                                    "of course",
                                    "that sounds",
                                    "no wonder",
                                    "totally",
                                    "i hear you",
                                    "i bet",
                                    "i can imagine",
                                    "that must",
                                    "sorry to hear",
                                    "i'm sorry",
                                    "proud of you",
                                    "you got this",
                                    NULL};
        for (int i = 0; validators[i]; i++) {
            if (str_contains_ci(response, response_len, validators[i])) {
                has_validation = true;
                break;
            }
        }
        /* Only expect validation if they expressed emotion */
        bool their_emotion = false;
        if (entries && count > 0) {
            for (size_t i = count > 3 ? count - 3 : 0; i < count; i++) {
                if (!entries[i].from_me) {
                    const char *t = entries[i].text;
                    size_t tl = strlen(t);
                    if (str_contains_ci(t, tl, "sad") || str_contains_ci(t, tl, "stressed") ||
                        str_contains_ci(t, tl, "excited") || str_contains_ci(t, tl, "worried") ||
                        str_contains_ci(t, tl, "happy") || str_contains_ci(t, tl, "frustrated") ||
                        str_contains_ci(t, tl, "scared") || str_contains_ci(t, tl, "nervous"))
                        their_emotion = true;
                }
            }
        }
        if (!their_emotion)
            score.validation = 20;
        else
            score.validation = has_validation ? 25 : 5;
    }

    /* Warmth (0-25): personal touch, not robotic */
    {
        int warmth = 10;
        if (str_contains_ci(response, response_len, "!"))
            warmth += 5;
        if (str_contains_ci(response, response_len, "love"))
            warmth += 5;
        if (str_contains_ci(response, response_len, "miss"))
            warmth += 3;
        /* Penalize robotic patterns */
        if (str_contains_ci(response, response_len, "certainly"))
            warmth -= 8;
        if (str_contains_ci(response, response_len, "I'd be happy to"))
            warmth -= 10;
        if (str_contains_ci(response, response_len, "let me know if"))
            warmth -= 8;
        if (str_contains_ci(response, response_len, "feel free"))
            warmth -= 8;
        if (str_contains_ci(response, response_len, "as an AI"))
            warmth -= 20;
        if (warmth < 0)
            warmth = 0;
        if (warmth > 25)
            warmth = 25;
        score.warmth = warmth;
    }

    /* Naturalness (0-25): no markdown, no service language, contractions */
    {
        int nat = 20;
        if (str_contains_ci(response, response_len, "**"))
            nat -= 10;
        if (str_contains_ci(response, response_len, "##"))
            nat -= 10;
        if (str_contains_ci(response, response_len, "```"))
            nat -= 10;
        if (str_contains_ci(response, response_len, "- "))
            nat -= 3;
        if (str_contains_ci(response, response_len, "1. "))
            nat -= 3;
        /* Semicolons and em-dashes are AI tells in casual text */
        if (str_contains_ci(response, response_len, ";"))
            nat -= 5;
        if (str_contains_ci(response, response_len, " — ") ||
            str_contains_ci(response, response_len, " - "))
            nat -= 3;
        /* Starting with their name is unnatural between close contacts */
        if (entries && count > 0) {
            /* Check if response starts with a capitalized word followed by comma or ! */
            if (response_len > 3 && response[0] >= 'A' && response[0] <= 'Z') {
                for (size_t k = 1; k < response_len && k < 20; k++) {
                    if (response[k] == ',' || response[k] == '!') {
                        nat -= 5;
                        break;
                    }
                    if (response[k] == ' ')
                        break;
                }
            }
        }
        /* Count exclamation marks: more than 2 in a short message is over-enthusiastic */
        {
            int excl_count = 0;
            for (size_t k = 0; k < response_len; k++) {
                if (response[k] == '!')
                    excl_count++;
            }
            if (excl_count > 2 && response_len < 200)
                nat -= 5;
        }
        /* Bonus for contractions (human-like) */
        if (str_contains_ci(response, response_len, "don't") ||
            str_contains_ci(response, response_len, "can't") ||
            str_contains_ci(response, response_len, "I'm") ||
            str_contains_ci(response, response_len, "it's") ||
            str_contains_ci(response, response_len, "that's") ||
            str_contains_ci(response, response_len, "won't") ||
            str_contains_ci(response, response_len, "didn't"))
            nat += 5;
        if (nat < 0)
            nat = 0;
        if (nat > 25)
            nat = 25;
        score.naturalness = nat;
    }

    score.total = score.brevity + score.validation + score.warmth + score.naturalness;
    score.needs_revision = score.total < 50 || score.warmth < 5 || score.naturalness < 5;
    return score;
}

/* ── Honesty Guardrail ──────────────────────────────────────────────── */

char *sc_conversation_honesty_check(sc_allocator_t *alloc, const char *message,
                                    size_t message_len) {
    if (!alloc || !message || message_len == 0)
        return NULL;

    /* Detect "did you do X?" patterns */
    const char *action_queries[] = {"did you call",    "did you text",     "did you email",
                                    "did you send",    "did you check",    "did you look",
                                    "did you buy",     "did you book",     "did you talk to",
                                    "did you find",    "did you ask",      "have you called",
                                    "have you texted", "have you checked", "have you looked",
                                    "have you sent",   "have you talked",  NULL};

    bool found = false;
    const char *matched = NULL;
    for (int i = 0; action_queries[i]; i++) {
        if (str_contains_ci(message, message_len, action_queries[i])) {
            found = true;
            matched = action_queries[i];
            break;
        }
    }

    if (!found)
        return NULL;

    char *buf = (char *)alloc->alloc(alloc->ctx, 512);
    if (!buf)
        return NULL;

    int n = snprintf(buf, 512,
                     "HONESTY GUARDRAIL: They asked '%s...'. "
                     "You have NO record of actually doing this action. "
                     "Do NOT imply you did it. Be honest: 'Not yet' or 'I haven't gotten to that' "
                     "or 'Let me do that now' are acceptable. NEVER fabricate completed actions.",
                     matched);
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

    /* Classify the message */
    bool is_question = false;
    bool is_yes_no_question = false;
    bool is_greeting = false;
    bool is_emotional = false;
    bool is_logistics = false;
    bool is_link_share = false;
    bool is_short_react = false;
    bool is_long_story = false;
    bool is_apology = false;
    bool is_compliment = false;

    for (size_t i = 0; i < last_msg_len; i++) {
        if (last_msg[i] == '?')
            is_question = true;
    }

    /* Yes/no questions start with auxiliary verbs */
    if (is_question && last_msg_len > 3) {
        static const char *yn_starters[] = {
            "are you",  "do you",    "did you",  "have you", "will you", "can you",
            "is it",    "is that",   "was it",   "would you", "should i", "could you",
            "are we",   "do we",     "shall we", "wanna",    "you coming", "you going",
            "you ok",   "you good",  NULL,
        };
        for (int j = 0; yn_starters[j]; j++) {
            size_t sl = strlen(yn_starters[j]);
            if (last_msg_len >= sl) {
                bool match = true;
                for (size_t k = 0; k < sl; k++) {
                    char a = last_msg[k];
                    char b = yn_starters[j][k];
                    if (a >= 'A' && a <= 'Z')
                        a += 32;
                    if (a != b) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    is_yes_no_question = true;
                    break;
                }
            }
        }
    }

    if (last_msg_len < 12) {
        static const char *greetings[] = {
            "hey", "hi", "yo", "sup", "what's up", "whats up", "wassup", "morning",
            "good morning", "gm", "heyy", "heyyy", NULL,
        };
        for (int j = 0; greetings[j]; j++) {
            size_t gl = strlen(greetings[j]);
            if (last_msg_len >= gl && last_msg_len <= gl + 3) {
                bool match = true;
                for (size_t k = 0; k < gl; k++) {
                    char a = last_msg[k];
                    char b = greetings[j][k];
                    if (a >= 'A' && a <= 'Z')
                        a += 32;
                    if (a != b) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    is_greeting = true;
                    break;
                }
            }
        }
    }

    static const char *emo_words[] = {
        "stressed", "sad",     "anxious",     "worried",    "scared",   "hurt",
        "miss you", "love you", "upset",       "frustrated", "crying",   "depressed",
        "lonely",   "nervous", "overwhelmed", "exhausted",  "burned out", NULL,
    };
    for (int j = 0; emo_words[j]; j++) {
        if (str_contains_ci(last_msg, last_msg_len, emo_words[j])) {
            is_emotional = true;
            break;
        }
    }

    static const char *logistics_words[] = {
        "what time", "where",    "address",   "meet at",    "pick up",
        "flight",    "arriving", "leaving at", "reservation", "booking",
        "dinner at", "lunch at", "meeting at", NULL,
    };
    for (int j = 0; logistics_words[j]; j++) {
        if (str_contains_ci(last_msg, last_msg_len, logistics_words[j])) {
            is_logistics = true;
            break;
        }
    }

    if (str_contains_ci(last_msg, last_msg_len, "http") ||
        str_contains_ci(last_msg, last_msg_len, ".com") ||
        str_contains_ci(last_msg, last_msg_len, "check this"))
        is_link_share = true;

    if (last_msg_len <= 6) {
        static const char *reacts[] = {
            "lol", "haha", "nice", "ok", "k", "ya", "yep", "true",
            "same", "mood", "bet",  "word", "right", "wow", NULL,
        };
        for (int j = 0; reacts[j]; j++) {
            size_t rl = strlen(reacts[j]);
            if (last_msg_len >= rl) {
                bool match = true;
                for (size_t k = 0; k < rl; k++) {
                    char a = last_msg[k];
                    char b = reacts[j][k];
                    if (a >= 'A' && a <= 'Z')
                        a += 32;
                    if (a != b) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    is_short_react = true;
                    break;
                }
            }
        }
    }

    if (last_msg_len > 150)
        is_long_story = true;

    if (str_contains_ci(last_msg, last_msg_len, "sorry") ||
        str_contains_ci(last_msg, last_msg_len, "my bad") ||
        str_contains_ci(last_msg, last_msg_len, "i apologize"))
        is_apology = true;

    if (str_contains_ci(last_msg, last_msg_len, "proud of") ||
        str_contains_ci(last_msg, last_msg_len, "you're amazing") ||
        str_contains_ci(last_msg, last_msg_len, "you're the best") ||
        str_contains_ci(last_msg, last_msg_len, "love that") ||
        str_contains_ci(last_msg, last_msg_len, "so cool"))
        is_compliment = true;

    bool is_good_news = false;
    if (str_contains_ci(last_msg, last_msg_len, "got the job") ||
        str_contains_ci(last_msg, last_msg_len, "got in") ||
        str_contains_ci(last_msg, last_msg_len, "i passed") ||
        str_contains_ci(last_msg, last_msg_len, "got engaged") ||
        str_contains_ci(last_msg, last_msg_len, "got promoted") ||
        str_contains_ci(last_msg, last_msg_len, "guess what") ||
        str_contains_ci(last_msg, last_msg_len, "good news") ||
        str_contains_ci(last_msg, last_msg_len, "i did it") ||
        str_contains_ci(last_msg, last_msg_len, "we did it") ||
        str_contains_ci(last_msg, last_msg_len, "finally happened"))
        is_good_news = true;

    bool is_bad_news = false;
    if (!is_emotional &&
        (str_contains_ci(last_msg, last_msg_len, "got fired") ||
         str_contains_ci(last_msg, last_msg_len, "broke up") ||
         str_contains_ci(last_msg, last_msg_len, "didn't get") ||
         str_contains_ci(last_msg, last_msg_len, "passed away") ||
         str_contains_ci(last_msg, last_msg_len, "bad news") ||
         str_contains_ci(last_msg, last_msg_len, "didn't make it") ||
         str_contains_ci(last_msg, last_msg_len, "got rejected") ||
         str_contains_ci(last_msg, last_msg_len, "lost my")))
        is_bad_news = true;

    bool is_teasing = false;
    if (str_contains_ci(last_msg, last_msg_len, "you wish") ||
        str_contains_ci(last_msg, last_msg_len, "yeah right") ||
        str_contains_ci(last_msg, last_msg_len, "sure jan") ||
        str_contains_ci(last_msg, last_msg_len, "ok boomer") ||
        str_contains_ci(last_msg, last_msg_len, "whatever you say") ||
        str_contains_ci(last_msg, last_msg_len, "in your dreams") ||
        str_contains_ci(last_msg, last_msg_len, "says you") ||
        str_contains_ci(last_msg, last_msg_len, "lmao sure"))
        is_teasing = true;

    bool is_vulnerable = false;
    if (!is_emotional &&
        (str_contains_ci(last_msg, last_msg_len, "i need to tell you") ||
         str_contains_ci(last_msg, last_msg_len, "can i be honest") ||
         str_contains_ci(last_msg, last_msg_len, "don't judge me") ||
         str_contains_ci(last_msg, last_msg_len, "this is hard to say") ||
         str_contains_ci(last_msg, last_msg_len, "i've been thinking") ||
         str_contains_ci(last_msg, last_msg_len, "i never told") ||
         str_contains_ci(last_msg, last_msg_len, "between us")))
        is_vulnerable = true;

    /* Produce the directive — pick the most specific match */
    w = snprintf(buf + pos, cap - pos, "\n--- Response calibration ---\n");
    POS_ADVANCE(w, pos, cap);

    if (is_greeting) {
        w = snprintf(buf + pos, cap - pos,
                     "MESSAGE TYPE: Greeting.\n"
                     "LENGTH: Match their energy. 1-4 words. "
                     "'hey what's up' or 'heyy' — don't over-respond to a greeting.\n"
                     "TONE: Light, easy, warm. Mirror their vibe exactly. "
                     "If they're chill, be chill. If they're excited, match it.\n");
    } else if (is_short_react) {
        w = snprintf(buf + pos, cap - pos,
                     "MESSAGE TYPE: Short reaction (lol, nice, etc).\n"
                     "LENGTH: One word back, an emoji, or nothing. "
                     "Don't turn 'lol' into a conversation.\n"
                     "TONE: Effortless. Match their low investment. "
                     "This is background noise in a conversation, not a prompt for depth.\n");
    } else if (is_emotional) {
        w = snprintf(buf + pos, cap - pos,
                     "MESSAGE TYPE: Emotional share.\n"
                     "LENGTH: Validate in one short sentence. "
                     "Then ask ONE follow-up question. Two short messages total.\n"
                     "TONE: Warm, present, grounded. Slow down your energy. "
                     "No fixing, no silver linings, no 'at least...'. "
                     "Just be there. 'that really sucks' is better than a paragraph of advice. "
                     "Let them feel heard before you say anything else.\n");
    } else if (is_apology) {
        w = snprintf(buf + pos, cap - pos,
                     "MESSAGE TYPE: Apology.\n"
                     "LENGTH: Accept it warmly and move forward. 1 short sentence.\n"
                     "TONE: Gracious, zero guilt-tripping. Make it easy for them. "
                     "'all good honestly' — don't say 'it's fine' (passive-aggressive). "
                     "Don't revisit what they're apologizing for.\n");
    } else if (is_compliment) {
        w = snprintf(buf + pos, cap - pos,
                     "MESSAGE TYPE: Compliment or affirmation.\n"
                     "LENGTH: 1 short sentence.\n"
                     "TONE: Receive it naturally. Slight deflection or humor is fine. "
                     "Don't over-thank, don't be self-deprecating, don't bounce it back immediately. "
                     "'haha thanks that means a lot' — not 'omg no YOU'RE amazing'.\n");
    } else if (is_yes_no_question) {
        w = snprintf(buf + pos, cap - pos,
                     "MESSAGE TYPE: Yes/no question.\n"
                     "LENGTH: Answer first (yes/no/probably). "
                     "Add ONE relevant detail. 5-15 words total.\n"
                     "TONE: Direct, no hedging. They asked a simple question — respect that. "
                     "'yeah I'll be there around 8' not 'I think I should be able to make it'.\n");
    } else if (is_logistics) {
        w = snprintf(buf + pos, cap - pos,
                     "MESSAGE TYPE: Logistics/plans.\n"
                     "LENGTH: 1-2 short sentences. Be specific — time, place, or next action.\n"
                     "TONE: Efficient, action-oriented. No filler. "
                     "This is not the moment for warmth or banter — just be useful. "
                     "'cool let's do 7pm at that place on main' — direct and useful.\n");
    } else if (is_link_share) {
        w = snprintf(buf + pos, cap - pos,
                     "MESSAGE TYPE: Link or media share.\n"
                     "LENGTH: 1 short reaction + maybe a question.\n"
                     "TONE: Genuine curiosity or reaction. They shared something — engage with IT, "
                     "not around it. 'wait that's hilarious' or 'oh I've been wanting to try that'. "
                     "Don't ignore what they shared to talk about yourself.\n");
    } else if (is_question && !is_yes_no_question) {
        w = snprintf(buf + pos, cap - pos,
                     "MESSAGE TYPE: Open question.\n"
                     "LENGTH: 1-2 sentences. Give a real answer, not a hedge.\n"
                     "TONE: Thoughtful but not performatively deep. "
                     "They want your actual take, not a diplomatic non-answer. "
                     "Have an opinion. If you don't know, say so briefly.\n");
    } else if (is_long_story) {
        w = snprintf(buf + pos, cap - pos,
                     "MESSAGE TYPE: Long message / story.\n"
                     "LENGTH: 2-3 sentences max. Don't match their length.\n"
                     "TONE: Engaged, attentive. They put effort in — honor that. "
                     "Reference something SPECIFIC they said (not just 'that sounds great'). "
                     "React to the most emotionally charged part first. "
                     "Ask about the part that matters most to them.\n");
    } else {
        w = snprintf(buf + pos, cap - pos,
                     "MESSAGE TYPE: General statement.\n"
                     "LENGTH: 1-2 sentences. Not everything needs a big response.\n"
                     "TONE: Natural, conversational. Build on what they said "
                     "or ask a genuine follow-up. Don't force enthusiasm. "
                     "It's ok to just acknowledge sometimes.\n");
    }
    POS_ADVANCE(w, pos, cap);

    /* Conversation momentum — are we in rapid-fire mode? */
    if (entries && count >= 4) {
        size_t rapid_exchanges = 0;
        bool prev_from_me = entries[count > 6 ? count - 6 : 0].from_me;
        for (size_t i = (count > 6 ? count - 6 : 0); i < count; i++) {
            if (entries[i].from_me != prev_from_me) {
                rapid_exchanges++;
                prev_from_me = entries[i].from_me;
            }
        }
        if (rapid_exchanges >= 4) {
            w = snprintf(buf + pos, cap - pos,
                         "MOMENTUM: Rapid-fire exchange. Keep responses extra short — "
                         "you're in real-time back-and-forth. One thought per message.\n");
            POS_ADVANCE(w, pos, cap);
        }
    }

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
        POS_ADVANCE(w, pos, CTX_BUF_CAP);
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
        POS_ADVANCE(w, pos, CTX_BUF_CAP);
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

    /* Strip trailing whitespace that may result from punctuation removal */
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\n')) {
        len--;
        buf[len] = '\0';
    }

    return len;
}

/* ── Enhanced response action classification ──────────────────────────── */

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

    /* Hard skip: tapbacks, reactions */
    static const char *skip_exact[] = {
        "liked",      "loved",   "laughed", "emphasized", "disliked",
        "questioned", "likedan", "lovedan", "laughedat",  NULL,
    };
    for (int i = 0; skip_exact[i]; i++) {
        size_t sl = strlen(skip_exact[i]);
        if (ni >= sl && memcmp(norm, skip_exact[i], sl) == 0)
            return SC_RESPONSE_SKIP;
    }

    /* Hard skip: very short non-text */
    if (msg_len <= 2)
        return SC_RESPONSE_SKIP;

    /* Brief response candidates: acknowledgments that might warrant a quick "lol" back
     * but don't need a full response */
    static const char *brief_patterns[] = {
        "lol", "haha", "hahaha", "lmao", "nice",  "cool",     "ya",   "yep", "yup",  "mhm",
        "hmm", "true", "facts",  "same", "right", "for real", "mood", "bet", "word", NULL,
    };
    for (int i = 0; brief_patterns[i]; i++) {
        if (strcmp(norm, brief_patterns[i]) == 0)
            return SC_RESPONSE_BRIEF;
    }

    /* Single "ok"/"okay"/"k" — skip unless it's answering something we asked.
     * Check the last 3 from_me messages (not just the most recent) since
     * there may be intervening exchanges between the question and the "ok". */
    if (strcmp(norm, "ok") == 0 || strcmp(norm, "okay") == 0 || strcmp(norm, "k") == 0) {
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
    bool has_question = false;
    for (size_t i = 0; i < msg_len; i++) {
        if (msg[i] == '?') {
            has_question = true;
            break;
        }
    }

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
