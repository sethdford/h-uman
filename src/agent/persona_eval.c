/* PersonaEval v2 — C port of scripts/personaeval_speaker_id_v2.py.
 *
 * featurize_v2 + sigmoid + classify_text, in-process, ~10 µs per call.
 * Required for Round 5 (production p_seth_at_send) and Round 6
 * (meta-cognitive uncertainty routing). See:
 *   docs/plans/2026-05-19-vision-better-than-human.md Round 5
 *   docs/plans/2026-05-19-agi-path.md Capability-2
 *
 * Parity contract: for any input, this should produce the same P(Seth)
 * as the Python v2 classifier to ~6 decimal places. Pinned by
 * tests/test_persona_eval.c which loads the same model JSON and
 * compares against hand-computed reference values. */
#include "human/agent/persona_eval.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Model struct — opaque to callers. The arrays are heap-owned, freed
 * by hu_persona_eval_free. */
struct hu_persona_eval_model {
    size_t n_features;
    char **feature_names; /* n_features × C-string (heap) */
    double *means;        /* n_features */
    double *stds;         /* n_features */
    double *weights;      /* n_features */
    double bias;
    /* Quick-lookup index: same length as n_features. Populated at
     * load time so featurize doesn't need to strcmp on every score. */
    int *feature_id; /* maps feature_names[i] → enum below */
};

/* The 20 features in the v2 model. Order MUST stay stable; the model
 * file references them BY NAME but we hot-load the name→id mapping
 * once at load time. */
enum {
    F_AVG_WORD_LEN,
    F_CAPITAL_WORD_RATIO,
    F_CONTRACTION_DENSITY,
    F_ELLIPSIS_DENSITY,
    F_ENDS_WITH_PERIOD,
    F_ENDS_WITH_Q,
    F_EXCLAM_DENSITY,
    F_HAS_BOLD,
    F_HAS_BULLET,
    F_HAS_CONTRACTION,
    F_HAS_HEADER,
    F_HAS_LOL_OR_HA,
    F_HAS_NUMBERED,
    F_IS_AI_OPENER,
    F_IS_SETH_OPENER,
    F_LEN_CHARS,
    F_LEN_WORDS,
    F_LOWERCASE_RATIO,
    F_N_SENTENCES,
    F_WORDS_PER_MSG,
    F_COUNT
};

static const char *FEATURE_NAME_TABLE[F_COUNT] = {
    "avg_word_len",     "capital_word_ratio", "contraction_density", "ellipsis_density",
    "ends_with_period", "ends_with_q",        "exclam_density",      "has_bold",
    "has_bullet",       "has_contraction",    "has_header",          "has_lol_or_ha",
    "has_numbered",     "is_ai_opener",       "is_seth_opener",      "len_chars",
    "len_words",        "lowercase_ratio",    "n_sentences",         "words_per_msg",
};

static int name_to_id(const char *name) {
    for (int i = 0; i < F_COUNT; i++) {
        if (strcmp(name, FEATURE_NAME_TABLE[i]) == 0)
            return i;
    }
    return -1;
}

/* AI/Seth opener token sets — copied verbatim from the Python source
 * to keep parity. Match against the first token after stripping
 * trailing ,.!:;. */
static bool token_in_set(const char *tok, const char *const *set, size_t set_n) {
    for (size_t i = 0; i < set_n; i++) {
        if (strcmp(tok, set[i]) == 0)
            return true;
    }
    return false;
}

static const char *AI_OPENERS[] = {"depending", "certainly", "absolutely", "great",
                                   "of",        "i",         "here",       "that"};
static const size_t AI_OPENERS_N = sizeof(AI_OPENERS) / sizeof(AI_OPENERS[0]);
static const char *SETH_OPENERS[] = {"yeah", "yo", "lol", "ha",   "damn", "nah", "wait", "ugh",
                                     "no",   "ok", "kk",  "fine", "sure", "hey", "hmm",  "real"};
static const size_t SETH_OPENERS_N = sizeof(SETH_OPENERS) / sizeof(SETH_OPENERS[0]);

/* Compute all 20 features for `text`. Writes into out[F_COUNT]. */
static void featurize_v2(const char *text, size_t text_len, double out[F_COUNT]) {
    for (int i = 0; i < F_COUNT; i++)
        out[i] = 0.0;
    if (!text || text_len == 0)
        return;

    size_t n_chars = text_len;
    size_t lower_chars = 0, upper_chars = 0;
    size_t exclam_count = 0;
    size_t ellipsis_count = 0; /* "..." runs */
    /* Word tokenization — match Python's text.split() (whitespace). */
    size_t n_words = 0;
    size_t total_word_chars = 0;
    size_t capital_words = 0;
    size_t i = 0;
    /* First-token capture for ai_opener / seth_opener */
    char first_token[64] = {0};
    bool got_first = false;
    /* For ellipsis count: count "..." runs (3+ consecutive dots). */
    size_t dot_run = 0;

    while (i < text_len) {
        /* Skip whitespace */
        while (i < text_len && isspace((unsigned char)text[i]))
            i++;
        if (i >= text_len)
            break;
        size_t wstart = i;
        while (i < text_len && !isspace((unsigned char)text[i]))
            i++;
        size_t wend = i;
        size_t wlen = wend - wstart;
        n_words++;
        total_word_chars += wlen;
        if (wlen >= 2) {
            char c0 = text[wstart];
            char c1 = text[wstart + 1];
            if (c0 >= 'A' && c0 <= 'Z' && (c1 >= 'a' && c1 <= 'z'))
                capital_words++;
        }
        if (!got_first) {
            size_t copy = wlen < sizeof(first_token) - 1 ? wlen : sizeof(first_token) - 1;
            for (size_t j = 0; j < copy; j++) {
                char c = text[wstart + j];
                first_token[j] = (char)tolower((unsigned char)c);
            }
            /* Strip trailing punctuation */
            while (copy > 0) {
                char tc = first_token[copy - 1];
                if (tc == ',' || tc == '.' || tc == '!' || tc == ':' || tc == ';') {
                    first_token[--copy] = '\0';
                } else
                    break;
            }
            first_token[copy] = '\0';
            got_first = true;
        }
    }

    /* Char-level scan: cases, exclam, ellipsis */
    dot_run = 0;
    for (size_t k = 0; k < text_len; k++) {
        unsigned char c = (unsigned char)text[k];
        if (islower(c))
            lower_chars++;
        else if (isupper(c))
            upper_chars++;
        if (c == '!')
            exclam_count++;
        if (c == '.') {
            dot_run++;
            if (dot_run == 3)
                ellipsis_count++;
        } else
            dot_run = 0;
    }

    /* Sentence count: re.split([.!?]+) on Python — equivalent to
     * counting runs of [.!?] as separators. We count NON-EMPTY
     * sentences as in Python (max 1, sum 1 for s if s.strip()). */
    size_t n_sentences = 0;
    bool in_text = false;
    for (size_t k = 0; k < text_len; k++) {
        unsigned char c = (unsigned char)text[k];
        bool is_sep = (c == '.' || c == '!' || c == '?');
        if (!is_sep && !isspace(c)) {
            if (!in_text) {
                in_text = true;
                n_sentences++;
            }
        } else if (is_sep) {
            in_text = false;
        }
    }
    if (n_sentences == 0)
        n_sentences = 1;

    /* Regex-style binary features. Conservative substring scans. */
    bool has_bullet = false, has_numbered = false, has_header = false, has_bold = false;
    for (size_t k = 0; k < text_len; k++) {
        if (k == 0 || text[k - 1] == '\n') {
            /* line start */
            size_t s = k;
            while (s < text_len && (text[s] == ' ' || text[s] == '\t'))
                s++;
            if (s < text_len) {
                if (text[s] == '*' || text[s] == '-') {
                    if (s + 1 < text_len && text[s + 1] == ' ')
                        has_bullet = true;
                }
                if (text[s] >= '0' && text[s] <= '9') {
                    size_t d = s;
                    while (d < text_len && text[d] >= '0' && text[d] <= '9')
                        d++;
                    if (d < text_len && text[d] == '.' && d + 1 < text_len && text[d + 1] == ' ')
                        has_numbered = true;
                }
                if (text[s] == '#') {
                    size_t h = s;
                    while (h < text_len && text[h] == '#' && (h - s) < 6)
                        h++;
                    if (h > s && h < text_len && text[h] == ' ')
                        has_header = true;
                }
            }
        }
        /* Bold: `**...**` with ≥2 chars between */
        if (k + 4 < text_len && text[k] == '*' && text[k + 1] == '*') {
            for (size_t m = k + 2; m + 1 < text_len; m++) {
                if (text[m] == '*' && text[m + 1] == '*' && m > k + 3) {
                    has_bold = true;
                    break;
                }
            }
        }
    }

    /* Contractions — substring match (case-insensitive). */
    static const char *contractions[] = {
        "don't", "i'm", "you're", "can't", "won't", "isn't", "gonna", "wanna", "kinda", "gotta",
    };
    bool has_contraction = false;
    size_t contraction_count = 0;
    for (size_t k = 0; k < text_len; k++) {
        for (size_t ci = 0; ci < sizeof(contractions) / sizeof(contractions[0]); ci++) {
            size_t clen = strlen(contractions[ci]);
            if (k + clen > text_len)
                continue;
            size_t j = 0;
            for (; j < clen; j++) {
                if (tolower((unsigned char)text[k + j]) != (unsigned char)contractions[ci][j])
                    break;
            }
            if (j == clen) {
                has_contraction = true;
                contraction_count++;
                break;
            }
        }
    }

    /* has_lol_or_ha: \b(lol|ha+)\b case-insensitive */
    bool has_lol_or_ha = false;
    for (size_t k = 0; k + 2 < text_len; k++) {
        bool left_ok = (k == 0) || !isalnum((unsigned char)text[k - 1]);
        if (!left_ok)
            continue;
        char c0 = (char)tolower((unsigned char)text[k]);
        char c1 = (char)tolower((unsigned char)text[k + 1]);
        char c2 = (char)tolower((unsigned char)text[k + 2]);
        if (c0 == 'l' && c1 == 'o' && c2 == 'l') {
            bool right_ok = (k + 3 == text_len) || !isalnum((unsigned char)text[k + 3]);
            if (right_ok) {
                has_lol_or_ha = true;
                break;
            }
        }
        if (c0 == 'h' && c1 == 'a') {
            /* ha+ : "ha" with optional trailing 'a's */
            size_t m = k + 2;
            while (m < text_len && tolower((unsigned char)text[m]) == 'a')
                m++;
            bool right_ok = (m == text_len) || !isalnum((unsigned char)text[m]);
            if (right_ok) {
                has_lol_or_ha = true;
                break;
            }
        }
    }

    /* ends_with_period / ends_with_q — after rstrip. */
    size_t end = text_len;
    while (end > 0 && isspace((unsigned char)text[end - 1]))
        end--;
    bool ends_period = end > 0 && text[end - 1] == '.';
    bool ends_q = end > 0 && text[end - 1] == '?';

    /* Opener classification */
    bool is_ai_op = false, is_seth_op = false;
    if (got_first) {
        is_ai_op = token_in_set(first_token, AI_OPENERS, AI_OPENERS_N);
        is_seth_op = token_in_set(first_token, SETH_OPENERS, SETH_OPENERS_N);
    }

    /* Pack into the output. Same field names as Python. */
    out[F_LEN_CHARS] = (double)n_chars;
    out[F_LEN_WORDS] = (double)n_words;
    out[F_N_SENTENCES] = (double)n_sentences;
    out[F_AVG_WORD_LEN] = n_words > 0 ? ((double)total_word_chars / n_words) : 0.0;
    size_t alpha = lower_chars + upper_chars;
    out[F_LOWERCASE_RATIO] = alpha > 0 ? ((double)lower_chars / alpha) : 0.0;
    out[F_HAS_BULLET] = has_bullet ? 1.0 : 0.0;
    out[F_HAS_NUMBERED] = has_numbered ? 1.0 : 0.0;
    out[F_HAS_HEADER] = has_header ? 1.0 : 0.0;
    out[F_HAS_BOLD] = has_bold ? 1.0 : 0.0;
    out[F_IS_AI_OPENER] = is_ai_op ? 1.0 : 0.0;
    out[F_IS_SETH_OPENER] = is_seth_op ? 1.0 : 0.0;
    out[F_HAS_CONTRACTION] = has_contraction ? 1.0 : 0.0;
    out[F_HAS_LOL_OR_HA] = has_lol_or_ha ? 1.0 : 0.0;
    out[F_ENDS_WITH_PERIOD] = ends_period ? 1.0 : 0.0;
    out[F_ENDS_WITH_Q] = ends_q ? 1.0 : 0.0;
    /* v2-added density features (normalized by word count) */
    double nw_d = n_words > 0 ? (double)n_words : 1.0;
    out[F_WORDS_PER_MSG] = (double)n_words;
    out[F_CAPITAL_WORD_RATIO] = (double)capital_words / nw_d;
    out[F_EXCLAM_DENSITY] = (double)exclam_count / nw_d;
    out[F_ELLIPSIS_DENSITY] = (double)ellipsis_count / nw_d;
    out[F_CONTRACTION_DENSITY] = (double)contraction_count / nw_d;
}

static double sigmoid(double z) {
    if (z < -50.0)
        return 0.0;
    if (z > 50.0)
        return 1.0;
    return 1.0 / (1.0 + exp(-z));
}

/* ── Public API ───────────────────────────────────────────────────────── */

static char *read_file_all(hu_allocator_t *alloc, const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 100 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return NULL;
    }
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

hu_error_t hu_persona_eval_load(hu_allocator_t *alloc, const char *path,
                                hu_persona_eval_model_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    const char *resolved = path ? path : "/tmp/seth_speaker_id.json";
    size_t bytes_len = 0;
    char *bytes = read_file_all(alloc, resolved, &bytes_len);
    if (!bytes) {
        hu_log_warn("persona_eval", NULL,
                    "model file unreadable: %s — P(Seth) scoring will "
                    "fall back to neutral 0.5",
                    resolved);
        return HU_ERR_IO;
    }

    hu_json_value_t *root = NULL;
    hu_error_t je = hu_json_parse(alloc, bytes, bytes_len, &root);
    alloc->free(alloc->ctx, bytes, bytes_len + 1);
    if (je != HU_OK || !root)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *fn = hu_json_object_get(root, "feature_names");
    hu_json_value_t *means = hu_json_object_get(root, "means");
    hu_json_value_t *stds = hu_json_object_get(root, "stds");
    hu_json_value_t *weights = hu_json_object_get(root, "weights");
    if (!fn || fn->type != HU_JSON_ARRAY || !means || !stds || !weights) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }
    size_t n = fn->data.array.len;
    if (n != F_COUNT || means->data.array.len != n || stds->data.array.len != n ||
        weights->data.array.len != n) {
        hu_log_warn("persona_eval", NULL, "model feature count mismatch (file=%zu, expected=%d)", n,
                    F_COUNT);
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_persona_eval_model_t *m = (hu_persona_eval_model_t *)alloc->alloc(alloc->ctx, sizeof(*m));
    if (!m) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(m, 0, sizeof(*m));
    m->n_features = n;
    m->means = (double *)alloc->alloc(alloc->ctx, n * sizeof(double));
    m->stds = (double *)alloc->alloc(alloc->ctx, n * sizeof(double));
    m->weights = (double *)alloc->alloc(alloc->ctx, n * sizeof(double));
    m->feature_id = (int *)alloc->alloc(alloc->ctx, n * sizeof(int));
    m->feature_names = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
    if (!m->means || !m->stds || !m->weights || !m->feature_id || !m->feature_names) {
        hu_persona_eval_free(alloc, m);
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(m->feature_names, 0, n * sizeof(char *));
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *name_v = fn->data.array.items[i];
        if (!name_v || name_v->type != HU_JSON_STRING) {
            hu_persona_eval_free(alloc, m);
            hu_json_free(alloc, root);
            return HU_ERR_INVALID_ARGUMENT;
        }
        size_t slen = name_v->data.string.len;
        m->feature_names[i] = (char *)alloc->alloc(alloc->ctx, slen + 1);
        if (!m->feature_names[i]) {
            hu_persona_eval_free(alloc, m);
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(m->feature_names[i], name_v->data.string.ptr, slen);
        m->feature_names[i][slen] = '\0';
        m->feature_id[i] = name_to_id(m->feature_names[i]);
        m->means[i] = means->data.array.items[i]->data.number;
        m->stds[i] = stds->data.array.items[i]->data.number;
        if (m->stds[i] < 1e-12)
            m->stds[i] = 1e-12;
        m->weights[i] = weights->data.array.items[i]->data.number;
    }
    hu_json_value_t *bias = hu_json_object_get(root, "bias");
    m->bias = (bias && bias->type == HU_JSON_NUMBER) ? bias->data.number : 0.0;

    hu_json_free(alloc, root);
    *out = m;
    return HU_OK;
}

void hu_persona_eval_free(hu_allocator_t *alloc, hu_persona_eval_model_t *m) {
    if (!alloc || !m)
        return;
    if (m->feature_names) {
        for (size_t i = 0; i < m->n_features; i++)
            if (m->feature_names[i])
                alloc->free(alloc->ctx, m->feature_names[i], strlen(m->feature_names[i]) + 1);
        alloc->free(alloc->ctx, m->feature_names, m->n_features * sizeof(char *));
    }
    if (m->means)
        alloc->free(alloc->ctx, m->means, m->n_features * sizeof(double));
    if (m->stds)
        alloc->free(alloc->ctx, m->stds, m->n_features * sizeof(double));
    if (m->weights)
        alloc->free(alloc->ctx, m->weights, m->n_features * sizeof(double));
    if (m->feature_id)
        alloc->free(alloc->ctx, m->feature_id, m->n_features * sizeof(int));
    alloc->free(alloc->ctx, m, sizeof(*m));
}

double hu_persona_eval_score(const hu_persona_eval_model_t *m, const char *text, size_t text_len) {
    if (!m)
        return 0.5; /* neutral when no model loaded */
    double feats[F_COUNT];
    featurize_v2(text, text_len, feats);
    double z = m->bias;
    for (size_t i = 0; i < m->n_features; i++) {
        int id = m->feature_id[i];
        if (id < 0 || id >= F_COUNT)
            continue;
        double xnorm = (feats[id] - m->means[i]) / m->stds[i];
        z += m->weights[i] * xnorm;
    }
    return sigmoid(z);
}

bool hu_persona_eval_is_seth(const hu_persona_eval_model_t *m, const char *text, size_t text_len,
                             double threshold) {
    return hu_persona_eval_score(m, text, text_len) >= threshold;
}
