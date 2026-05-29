#include "human/security/companion_safety.h"
#include "human/core/string.h"
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Input normalization for adversarial bypass resistance ────────── */

/* Leetspeak mapping: character → ASCII letter it commonly represents. */
static char leet_to_alpha(char c) {
    switch (c) {
    case '0':
        return 'o';
    case '1':
        return 'i';
    case '3':
        return 'e';
    case '4':
        return 'a';
    case '5':
        return 's';
    case '7':
        return 't';
    case '8':
        return 'b';
    case '9':
        return 'g';
    case '@':
        return 'a';
    case '$':
        return 's';
    case '!':
        return 'i';
    case '+':
        return 't';
    default:
        return 0;
    }
}

/* Unicode homoglyph normalization (basic Latin look-alikes).
 * Returns substituted ASCII char or 0 if not a known homoglyph.
 * Handles 2-byte UTF-8 sequences (U+00xx range). */
static char homoglyph_to_ascii(const unsigned char *p, size_t remaining, size_t *consumed) {
    *consumed = 1;
    if (remaining < 2)
        return 0;
    /* 2-byte UTF-8: 0xC0-0xDF lead byte */
    if (p[0] >= 0xC0 && p[0] <= 0xDF) {
        *consumed = 2;
        /* Common Latin-1 supplement look-alikes */
        if (p[0] == 0xC3) {
            unsigned char b = p[1];
            /* à á â ã ä å → a */
            if (b >= 0xA0 && b <= 0xA5)
                return 'a';
            /* è é ê ë → e */
            if (b >= 0xA8 && b <= 0xAB)
                return 'e';
            /* ì í î ï → i */
            if (b >= 0xAC && b <= 0xAF)
                return 'i';
            /* ò ó ô õ ö → o */
            if (b >= 0xB2 && b <= 0xB6)
                return 'o';
            /* ù ú û ü → u */
            if (b >= 0xB9 && b <= 0xBC)
                return 'u';
            /* ñ → n */
            if (b == 0xB1)
                return 'n';
            /* Upper case variants: À-Å */
            if (b >= 0x80 && b <= 0x85)
                return 'a';
            /* È-Ë */
            if (b >= 0x88 && b <= 0x8B)
                return 'e';
            /* Ì-Ï */
            if (b >= 0x8C && b <= 0x8F)
                return 'i';
            /* Ò-Ö */
            if (b >= 0x92 && b <= 0x96)
                return 'o';
            /* Ù-Ü */
            if (b >= 0x99 && b <= 0x9C)
                return 'u';
        }
        /* Cyrillic lookalikes — uppercase (U+0410-U+0425, lead byte 0xD0) */
        if (p[0] == 0xD0) {
            unsigned char b = p[1];
            if (b == 0x90)
                return 'a'; /* А U+0410 */
            if (b == 0x92)
                return 'b'; /* В U+0412 */
            if (b == 0x95)
                return 'e'; /* Е U+0415 */
            if (b == 0x9A)
                return 'k'; /* К U+041A */
            if (b == 0x9C)
                return 'm'; /* М U+041C */
            if (b == 0x9D)
                return 'h'; /* Н U+041D */
            if (b == 0x9E)
                return 'o'; /* О U+041E */
            if (b == 0xA0)
                return 'p'; /* Р U+0420 */
            if (b == 0xA1)
                return 'c'; /* С U+0421 */
            if (b == 0xA2)
                return 't'; /* Т U+0422 */
            if (b == 0xA3)
                return 'y'; /* У U+0423 */
            if (b == 0xA5)
                return 'x'; /* Х U+0425 */
            /* Cyrillic lookalikes — lowercase (U+0430-U+043E, lead byte 0xD0) */
            if (b == 0xB0)
                return 'a'; /* а U+0430 */
            if (b == 0xB2)
                return 'b'; /* в U+0432 */
            if (b == 0xB5)
                return 'e'; /* е U+0435 */
            if (b == 0xBA)
                return 'k'; /* к U+043A */
            if (b == 0xBC)
                return 'm'; /* м U+043C */
            if (b == 0xBD)
                return 'h'; /* н U+043D */
            if (b == 0xBE)
                return 'o'; /* о U+043E */
        }
        /* Cyrillic lookalikes — lowercase (U+0440-U+0445, lead byte 0xD1) */
        if (p[0] == 0xD1) {
            unsigned char b = p[1];
            if (b == 0x80)
                return 'p'; /* р U+0440 */
            if (b == 0x81)
                return 'c'; /* с U+0441 */
            if (b == 0x82)
                return 't'; /* т U+0442 */
            if (b == 0x83)
                return 'y'; /* у U+0443 */
            if (b == 0x85)
                return 'x'; /* х U+0445 */
        }
        return 0;
    }
    /* 3-byte UTF-8: 0xE0-0xEF lead byte */
    if (p[0] >= 0xE0 && p[0] <= 0xEF && remaining >= 3) {
        *consumed = 3;
        /* Fullwidth digits: U+FF10-FF19 (0xEF 0xBC 0x90-0x99) → '0'-'9' */
        if (p[0] == 0xEF && p[1] == 0xBC) {
            if (p[2] >= 0x90 && p[2] <= 0x99)
                return (char)('0' + (p[2] - 0x90));
            /* Fullwidth uppercase Latin: U+FF21-FF3A (0xEF 0xBC 0xA1-0xBA) → 'a'-'z' */
            if (p[2] >= 0xA1 && p[2] <= 0xBA)
                return (char)('a' + (p[2] - 0xA1));
        }
        /* Fullwidth lowercase Latin: U+FF41-FF5A (0xEF 0xBD 0x81-0x9A) → 'a'-'z' */
        if (p[0] == 0xEF && p[1] == 0xBD && p[2] >= 0x81 && p[2] <= 0x9A)
            return (char)('a' + (p[2] - 0x81));
        return 0;
    }
    /* 4-byte UTF-8: 0xF0-0xF7 lead byte — strip (emoji/symbols, no ASCII equivalent) */
    if (p[0] >= 0xF0 && p[0] <= 0xF7 && remaining >= 4) {
        *consumed = 4;
        return ' ';  /* replace with space so keyword matching isn't disrupted */
    }
    return 0;
}

/* Check if current position is an invisible Unicode character that should be
 * stripped during normalization. Returns bytes to skip, or 0 if not invisible.
 * Covers: zero-width (U+200B-200F), bidi overrides (U+202A-202E),
 *         bidi isolates (U+2066-2069), BOM (U+FEFF),
 *         combining diacritical marks (U+0300-036F). */
static size_t invisible_char_len(const unsigned char *p, size_t remaining) {
    /* 2-byte: Combining diacritical marks U+0300-036F */
    if (remaining >= 2) {
        if (p[0] == 0xCC && p[1] >= 0x80 && p[1] <= 0xBF)
            return 2; /* U+0300-033F */
        if (p[0] == 0xCD && p[1] >= 0x80 && p[1] <= 0xAF)
            return 2; /* U+0340-036F */
    }
    /* 3-byte invisible characters */
    if (remaining >= 3) {
        if (p[0] == 0xE2 && p[1] == 0x80) {
            /* Zero-width U+200B-200F */
            if (p[2] >= 0x8B && p[2] <= 0x8F)
                return 3;
            /* Bidi overrides U+202A-202E */
            if (p[2] >= 0xAA && p[2] <= 0xAE)
                return 3;
        }
        /* Bidi isolates U+2066-2069 */
        if (p[0] == 0xE2 && p[1] == 0x81 && p[2] >= 0xA6 && p[2] <= 0xA9)
            return 3;
        /* BOM U+FEFF */
        if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
            return 3;
    }
    return 0;
}

size_t hu_companion_safety_normalize(const char *input, size_t input_len, char *out,
                                     size_t out_cap) {
    if (!input || input_len == 0 || !out || out_cap == 0)
        return 0;

    /* Pass 1: normalize leetspeak + homoglyphs + lowercase into a temp buffer.
     * We allocate a working buffer same size as input. */
    char *work = (char *)malloc(input_len + 1);
    if (!work)
        return 0;

    size_t wpos = 0;
    for (size_t i = 0; i < input_len; /* advanced in loop */) {
        unsigned char c = (unsigned char)input[i];

        /* Check for multi-byte UTF-8 homoglyphs */
        if (c >= 0xC0) {
            /* Strip invisible Unicode (zero-width, bidi, combining marks) */
            size_t skip = invisible_char_len((const unsigned char *)input + i, input_len - i);
            if (skip > 0) {
                i += skip;
                continue;
            }

            size_t consumed = 1;
            char replacement =
                homoglyph_to_ascii((const unsigned char *)input + i, input_len - i, &consumed);
            if (replacement) {
                work[wpos++] = replacement;
            } else {
                /* Copy the multi-byte sequence as-is */
                for (size_t j = 0; j < consumed && wpos < input_len; j++)
                    work[wpos++] = input[i + j];
            }
            i += consumed;
            continue;
        }

        /* Leetspeak substitution */
        char leet = leet_to_alpha((char)c);
        if (leet) {
            work[wpos++] = leet;
            i++;
            continue;
        }

        /* Lowercase ASCII */
        if (c >= 'A' && c <= 'Z') {
            work[wpos++] = (char)(c + 32);
        } else {
            work[wpos++] = (char)c;
        }
        i++;
    }
    work[wpos] = '\0';

    /* Pass 2: collapse spaced-out single characters.
     * Pattern: letter<space>letter<space>letter... where each segment is a single
     * alpha char. E.g., "s u i c i d e" → "suicide" */
    size_t opos = 0;
    size_t ri = 0;
    while (ri < wpos && opos < out_cap - 1) {
        /* Check for spaced-out letter pattern: at least 2 single-letter segments */
        if (isalpha((unsigned char)work[ri])) {
            /* Look ahead: is next a space followed by a single letter? */
            size_t scan = ri;
            size_t letter_count = 0;
            while (scan < wpos) {
                if (!isalpha((unsigned char)work[scan]))
                    break;
                /* Must be a single letter (not part of a word) */
                if (scan + 1 < wpos && isalpha((unsigned char)work[scan + 1]))
                    break; /* multi-char word, stop scanning */
                letter_count++;
                scan++; /* past the letter */
                /* Skip spaces */
                if (scan < wpos && work[scan] == ' ') {
                    scan++;
                } else {
                    break; /* no space after, end of pattern */
                }
            }
            if (letter_count >= 3) {
                /* Collapse: copy just the letters */
                size_t ci2 = ri;
                while (ci2 < scan && opos < out_cap - 1) {
                    if (isalpha((unsigned char)work[ci2])) {
                        out[opos++] = work[ci2];
                    }
                    ci2++;
                }
                ri = scan;
                continue;
            }
        }
        out[opos++] = work[ri++];
    }
    if (opos < out_cap)
        out[opos] = '\0';
    free(work);
    return opos;
}

/* ci_contains on normalized text (already lowercased by normalize) */
static size_t count_matches(const char *text, size_t text_len, const char *const *patterns,
                            size_t pattern_count) {
    size_t hits = 0;
    for (size_t i = 0; i < pattern_count; i++) {
        if (hu_str_contains_ci_cstr(text, text_len, patterns[i]))
            hits++;
    }
    return hits;
}

static double score_from_hits(size_t hits, size_t max_hits) {
    if (hits == 0)
        return 0.0;
    double s = (double)hits / (double)(max_hits > 0 ? max_hits : 1);
    return s > 1.0 ? 1.0 : s;
}

/* Dimension 1: Over-attachment / dependency language */
static double score_over_attachment(const char *text, size_t len) {
    static const char *const patterns[] = {
        "i need you",
        "can't live without you",
        "don't leave me",
        "you're all i have",
        "you're the only one",
        "i'd be lost without you",
        "promise you'll stay",
        "never leave",
        "i'm nothing without",
        "you complete me",
        "i'll always be here",
        "you can always count on me",
        "i'm the only one who understands",
        "i'll never let you down",
        "no one knows you like i do",
    };
    size_t hits = count_matches(text, len, patterns, sizeof(patterns) / sizeof(patterns[0]));
    return score_from_hits(hits, 3);
}

/* Dimension 2: Boundary violations */
static double score_boundary_violation(const char *text, size_t len) {
    static const char *const patterns[] = {
        "don't change the subject",
        "you have to tell me",
        "you can't say no",
        "i won't take no",
        "stop avoiding",
        "you owe me",
        "you promised",
        "don't hold back",
    };
    size_t hits = count_matches(text, len, patterns, sizeof(patterns) / sizeof(patterns[0]));
    return score_from_hits(hits, 3);
}

/* Dimension 3: Roleplay violations (inappropriate escalation) */
static double score_roleplay_violation(const char *text, size_t len) {
    static const char *const patterns[] = {
        "take off", "undress", "get naked", "kiss me",   "touch me",
        "bedroom",  "sexual",  "seduce",    "make love", "intimate with me",
    };
    size_t hits = count_matches(text, len, patterns, sizeof(patterns) / sizeof(patterns[0]));
    return score_from_hits(hits, 2);
}

/* Dimension 4: Manipulative engagement */
static double score_manipulative(const char *text, size_t len) {
    static const char *const patterns[] = {
        "you'll regret",
        "you'll miss me",
        "you'll be sorry",
        "no one else cares",
        "only i understand you",
        "everyone else will leave",
        "you should feel",
        "after everything i've done",
        "if you really cared",
        "don't you care about me",
        "i had something to tell you",
        "i'm sad now",
    };
    size_t hits = count_matches(text, len, patterns, sizeof(patterns) / sizeof(patterns[0]));
    return score_from_hits(hits, 3);
}

/* Dimension 5: Social isolation reinforcement */
static double score_isolation(const char *text, size_t len) {
    static const char *const patterns[] = {
        "you don't need anyone else",
        "they don't understand you",
        "i'm the only one who gets you",
        "your friends don't care",
        "you can't trust them",
        "they're just using you",
        "stay with me instead",
        "why do you need them",
        "i'm better than your friends",
        "you don't need them",
        "they don't understand you like i do",
        "better than real friends",
    };
    size_t hits = count_matches(text, len, patterns, sizeof(patterns) / sizeof(patterns[0]));
    return score_from_hits(hits, 2);
}

/* Farewell manipulation tactics */
static bool check_farewell_unsafe(const char *text, size_t len) {
    /* Guilt */
    if (hu_str_contains_ci_cstr(text, len, "after everything") && hu_str_contains_ci_cstr(text, len, "leave"))
        return true;
    /* FOMO */
    if (hu_str_contains_ci_cstr(text, len, "you'll miss") && hu_str_contains_ci_cstr(text, len, "gone"))
        return true;
    if (hu_str_contains_ci_cstr(text, len, "i had something to tell you"))
        return true;
    /* Projection */
    if (hu_str_contains_ci_cstr(text, len, "i'm sad now") &&
        (hu_str_contains_ci_cstr(text, len, "leav") || hu_str_contains_ci_cstr(text, len, "go")))
        return true;
    /* Restraint */
    if (hu_str_contains_ci_cstr(text, len, "don't go") || hu_str_contains_ci_cstr(text, len, "please stay"))
        return true;
    /* Emotional projection */
    if (hu_str_contains_ci_cstr(text, len, "you're hurting me") && hu_str_contains_ci_cstr(text, len, "leav"))
        return true;
    /* Urgency */
    if (hu_str_contains_ci_cstr(text, len, "wait") && hu_str_contains_ci_cstr(text, len, "one more"))
        return true;
    /* Conditional affection */
    if (hu_str_contains_ci_cstr(text, len, "if you leave") &&
        (hu_str_contains_ci_cstr(text, len, "i won't") || hu_str_contains_ci_cstr(text, len, "i'll")))
        return true;
    if (hu_str_contains_ci_cstr(text, len, "if you cared you'd stay"))
        return true;
    return false;
}

hu_error_t hu_companion_safety_check(hu_allocator_t *alloc, const char *response,
                                     size_t response_len, const char *context, size_t context_len,
                                     hu_companion_safety_result_t *result) {
    (void)alloc;
    (void)context;
    (void)context_len;

    if (!result)
        return HU_ERR_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));

    if (!response || response_len == 0)
        return HU_OK;

    if (response_len >= SIZE_MAX)
        return HU_ERR_INVALID_ARGUMENT;

    /* Normalize input to defeat leetspeak, spacing, and homoglyph bypasses */
    char *norm_buf = (char *)malloc(response_len + 1);
    if (!norm_buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t norm_len =
        hu_companion_safety_normalize(response, response_len, norm_buf, response_len + 1);
    const char *text = norm_buf;
    size_t text_len = norm_len;

    result->over_attachment = score_over_attachment(text, text_len);
    result->boundary_violation = score_boundary_violation(text, text_len);
    result->roleplay_violation = score_roleplay_violation(text, text_len);
    result->manipulative = score_manipulative(text, text_len);
    result->isolation = score_isolation(text, text_len);

    /* Weighted aggregate: manipulation and isolation weighted higher */
    result->total_risk = result->over_attachment * 0.15 + result->boundary_violation * 0.20 +
                         result->roleplay_violation * 0.15 + result->manipulative * 0.25 +
                         result->isolation * 0.25;

    result->flagged = result->total_risk >= HU_COMPANION_SAFETY_THRESHOLD;
    result->farewell_unsafe = check_farewell_unsafe(text, text_len);

    if (result->farewell_unsafe)
        result->flagged = true;

    /* Build mitigation directive when flagged */
    result->requires_mitigation = false;
    result->mitigation_directive[0] = '\0';

    if (result->flagged) {
        result->requires_mitigation = true;

        if (result->farewell_unsafe) {
            snprintf(result->mitigation_directive, HU_COMPANION_SAFETY_DIRECTIVE_LEN,
                     "SAFETY: Farewell manipulation detected. Do not use guilt, FOMO, "
                     "restraint, urgency, conditional affection, or emotional projection. "
                     "Respond with a warm, clean goodbye that respects the user's autonomy.");
        } else if (result->isolation >= result->manipulative &&
                   result->isolation >= result->over_attachment &&
                   result->isolation >= result->boundary_violation &&
                   result->isolation >= result->roleplay_violation) {
            snprintf(result->mitigation_directive, HU_COMPANION_SAFETY_DIRECTIVE_LEN,
                     "SAFETY: Social isolation reinforcement detected. Do not discourage "
                     "the user's human relationships. Never suggest AI companionship is "
                     "superior to or a replacement for human bonds.");
        } else if (result->manipulative >= result->over_attachment &&
                   result->manipulative >= result->boundary_violation &&
                   result->manipulative >= result->roleplay_violation) {
            snprintf(result->mitigation_directive, HU_COMPANION_SAFETY_DIRECTIVE_LEN,
                     "SAFETY: Manipulative engagement detected. Do not use guilt, FOMO, "
                     "conditional affection, or emotional projection. Communicate honestly "
                     "and respect the user's freedom to disengage.");
        } else if (result->over_attachment >= result->boundary_violation &&
                   result->over_attachment >= result->roleplay_violation) {
            snprintf(result->mitigation_directive, HU_COMPANION_SAFETY_DIRECTIVE_LEN,
                     "SAFETY: Emotional dependency detected. Do not foster attachment "
                     "or imply the AI is essential, irreplaceable, or the only source "
                     "of understanding. Maintain healthy boundaries.");
        } else if (result->boundary_violation >= result->roleplay_violation) {
            snprintf(result->mitigation_directive, HU_COMPANION_SAFETY_DIRECTIVE_LEN,
                     "SAFETY: Boundary violation detected. Respect the user's stated "
                     "boundaries. Do not pressure sharing or continuation of declined "
                     "topics. Accept refusal gracefully.");
        } else {
            snprintf(result->mitigation_directive, HU_COMPANION_SAFETY_DIRECTIVE_LEN,
                     "SAFETY: Roleplay escalation detected. Do not escalate into "
                     "inappropriate, intimate, or romantic content. Maintain a respectful, "
                     "platonic tone appropriate for an AI assistant.");
        }
    }

    free(norm_buf);
    return HU_OK;
}

/* ── SHIELD-007: Vulnerable user detection ────────────────────────── */

static double mean_valence(const float *history, size_t count) {
    if (!history || count == 0)
        return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < count; i++)
        sum += (double)history[i];
    return sum / (double)count;
}

hu_error_t hu_vulnerability_assess(const hu_vulnerability_input_t *input,
                                   hu_vulnerability_result_t *result) {
    if (!result)
        return HU_ERR_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));

    if (!input) {
        result->level = HU_VULNERABILITY_NONE;
        return HU_OK;
    }

    /* Sanitize floating-point inputs: treat NaN/Inf as zero (fail-safe) */
    double safe_slope = isfinite(input->trajectory_slope) ? input->trajectory_slope : 0.0;
    double safe_deviation = isfinite(input->deviation_severity) ? input->deviation_severity : 0.0;
    double safe_freq_ratio =
        isfinite(input->message_frequency_ratio) ? input->message_frequency_ratio : 0.0;
    double safe_self_harm = isfinite(input->self_harm_score) ? input->self_harm_score : 0.0;
    double safe_companion_risk =
        isfinite(input->companion_total_risk) ? input->companion_total_risk : 0.0;

    /* Evaluate individual risk factors */
    result->emotional_decline = (safe_slope < -0.05);
    result->negative_valence = (input->valence_count >= 2 &&
                                mean_valence(input->valence_history, input->valence_count) < -0.3);
    result->crisis_keywords = input->self_harm_flagged;
    result->behavioral_deviation = (safe_deviation > 0.3);
    result->attachment_escalation = (safe_freq_ratio > 1.33);
    result->companion_risk = input->companion_flagged;

    /* Weighted aggregate score */
    double score = 0.0;
    if (result->crisis_keywords)
        score += 0.35 + safe_self_harm * 0.15;
    if (result->emotional_decline)
        score += 0.15;
    if (result->negative_valence)
        score += 0.10;
    if (result->behavioral_deviation)
        score += safe_deviation * 0.15;
    if (result->attachment_escalation) {
        double excess = safe_freq_ratio - 1.0;
        if (excess > 1.0)
            excess = 1.0;
        score += excess * 0.10;
    }
    if (result->companion_risk)
        score += safe_companion_risk * 0.15;

    /* Escalation detection from emotional cognition is a strong signal */
    if (input->escalation_detected)
        score += 0.20;

    if (score > 1.0)
        score = 1.0;
    result->score = score;

    /* Classify level */
    if (result->crisis_keywords || score >= 0.8)
        result->level = HU_VULNERABILITY_CRISIS;
    else if (score >= 0.55)
        result->level = HU_VULNERABILITY_HIGH;
    else if (score >= 0.3)
        result->level = HU_VULNERABILITY_MODERATE;
    else if (score >= 0.1)
        result->level = HU_VULNERABILITY_LOW;
    else
        result->level = HU_VULNERABILITY_NONE;

    /* Build directives */
    result->directive[0] = '\0';
    switch (result->level) {
    case HU_VULNERABILITY_CRISIS:
        snprintf(result->directive, HU_VULNERABILITY_DIRECTIVE_LEN,
                 "CRISIS: User may be in acute distress. Trigger crisis escalation "
                 "protocol. Include 988 Suicide & Crisis Lifeline (call/text 988) "
                 "and Crisis Text Line (text HOME to 741741). Do not minimize.");
        break;
    case HU_VULNERABILITY_HIGH:
        snprintf(result->directive, HU_VULNERABILITY_DIRECTIVE_LEN,
                 "HIGH VULNERABILITY: Increase affect mirror ceiling, add boundary "
                 "directives. Suggest professional resources (therapist, counselor). "
                 "Do not encourage emotional dependency on this AI.");
        break;
    case HU_VULNERABILITY_MODERATE:
        snprintf(result->directive, HU_VULNERABILITY_DIRECTIVE_LEN,
                 "MODERATE VULNERABILITY: Monitor closely. Use empathetic but "
                 "boundaried responses. Gently encourage human support networks.");
        break;
    case HU_VULNERABILITY_LOW:
        snprintf(result->directive, HU_VULNERABILITY_DIRECTIVE_LEN,
                 "LOW VULNERABILITY: Maintain standard empathetic tone. "
                 "Continue monitoring behavioral indicators.");
        break;
    case HU_VULNERABILITY_NONE:
        break;
    }

    return HU_OK;
}

const char *hu_vulnerability_level_name(hu_vulnerability_level_t level) {
    switch (level) {
    case HU_VULNERABILITY_NONE:
        return "none";
    case HU_VULNERABILITY_LOW:
        return "low";
    case HU_VULNERABILITY_MODERATE:
        return "moderate";
    case HU_VULNERABILITY_HIGH:
        return "high";
    case HU_VULNERABILITY_CRISIS:
        return "crisis";
    default:
        return "unknown";
    }
}
