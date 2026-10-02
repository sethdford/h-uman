/* Learned Style Profile — runtime loader, lookup, renderer and length-rule
 * classifier. Contract: include/human/persona/learned_style.h.
 *
 * The file is numbers only (Part A refuses to write text), so nothing read
 * here can leak a message into a prompt or a log: the rendered line is built
 * from integers and fixed phrases, and the only name in it comes from the
 * hand-written persona contact, never from the file. */
#include "human/persona/learned_style.h"

#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/persona.h"
#include "human/persona/card_file.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ── shape rule ──────────────────────────────────────────────────────── */

#define HU_LS_STORY_BYTES     140
#define HU_LS_STORY_MIN_BYTES 80
#define HU_LS_STORY_MIN_RUNS  2

hu_ls_shape_t hu_learned_style_shape(const char *s, size_t len) {
    if (!s || len == 0)
        return HU_LS_SHAPE_CASUAL;
    size_t a = 0, b = len;
    while (a < b && isspace((unsigned char)s[a]))
        a++;
    while (b > a && isspace((unsigned char)s[b - 1]))
        b--;
    size_t n = b - a;
    if (n == 0)
        return HU_LS_SHAPE_CASUAL;
    if (memchr(s + a, '?', n))
        return HU_LS_SHAPE_QUESTION;
    if (n >= HU_LS_STORY_BYTES)
        return HU_LS_SHAPE_STORY;
    if (n >= HU_LS_STORY_MIN_BYTES) {
        size_t runs = 0;
        bool in_run = false;
        for (size_t i = a; i < b; i++) {
            bool term = s[i] == '.' || s[i] == '!';
            if (term && !in_run)
                runs++;
            in_run = term;
        }
        if (runs >= HU_LS_STORY_MIN_RUNS)
            return HU_LS_SHAPE_STORY;
    }
    return HU_LS_SHAPE_CASUAL;
}

const char *hu_learned_style_shape_name(hu_ls_shape_t shape) {
    switch (shape) {
    case HU_LS_SHAPE_QUESTION:
        return "question";
    case HU_LS_SHAPE_STORY:
        return "story";
    default:
        return "casual";
    }
}

hu_gate_mode_t hu_learned_style_mode(void) {
    return hu_gate_mode_from_env("HU_LEARNED_STYLE", HU_GATE_OFF);
}

/* ── cache ───────────────────────────────────────────────────────────── */

#define HU_LS_PATH_MAX 512
#define HU_LS_NAME_MAX 128
#define HU_LS_KEY_MAX  160

typedef struct {
    char persona[HU_LS_NAME_MAX];
    char path[HU_LS_PATH_MAX];
    bool have_stat;      /* a stat has been taken for `path` */
    time_t last_stat_at; /* clock() time of that stat */
    time_t mtime;
    off_t size;
    bool present;          /* file existed at the last stat */
    hu_json_value_t *root; /* validated document, or NULL (absent/malformed) */
} hu_ls_cache_t;

static pthread_mutex_t s_ls_mu = PTHREAD_MUTEX_INITIALIZER;
static hu_ls_cache_t s_ls;
static time_t (*s_ls_now)(void) = NULL;

static time_t ls_now(void) {
    return s_ls_now ? s_ls_now() : time(NULL);
}

void hu_learned_style_set_clock(time_t (*now_fn)(void)) {
    pthread_mutex_lock(&s_ls_mu);
    s_ls_now = now_fn;
    pthread_mutex_unlock(&s_ls_mu);
}

static void ls_drop_locked(void) {
    if (s_ls.root) {
        hu_allocator_t a = hu_system_allocator();
        hu_json_free(&a, s_ls.root);
    }
    char persona[HU_LS_NAME_MAX];
    memcpy(persona, s_ls.persona, sizeof(persona));
    memset(&s_ls, 0, sizeof(s_ls));
    memcpy(s_ls.persona, persona, sizeof(persona));
}

void hu_learned_style_cache_reset(void) {
    pthread_mutex_lock(&s_ls_mu);
    ls_drop_locked();
    pthread_mutex_unlock(&s_ls_mu);
}

void hu_learned_style_set_persona(const char *name, size_t name_len) {
    pthread_mutex_lock(&s_ls_mu);
    char next[HU_LS_NAME_MAX] = {0};
    if (name && name_len > 0 && name_len < sizeof(next))
        memcpy(next, name, name_len);
    if (strcmp(next, s_ls.persona) != 0) {
        ls_drop_locked();
        memcpy(s_ls.persona, next, sizeof(next));
    }
    pthread_mutex_unlock(&s_ls_mu);
}

/* Read obj[key] as a number in [lo, hi]. */
static bool ls_num(const hu_json_value_t *obj, const char *key, double lo, double hi, double *out) {
    const hu_json_value_t *v = hu_json_object_get(obj, key);
    if (!v || v->type != HU_JSON_NUMBER)
        return false;
    double d = v->data.number;
    if (!(d >= lo && d <= hi)) /* also rejects NaN */
        return false;
    *out = d;
    return true;
}

/* A stats object is all-or-nothing: one bad field and the level is skipped,
 * so a half-parsed row can never render a line from defaults. */
static bool ls_read_stats(const hu_json_value_t *o, hu_learned_style_t *out) {
    if (!o || o->type != HU_JSON_OBJECT)
        return false;
    double n, n_eff, p25, p50, p90, bub, low, emo, endp;
    if (!ls_num(o, "n", 0, 4.0e9, &n) || !ls_num(o, "n_eff", 0, 4.0e9, &n_eff) ||
        !ls_num(o, "len_p25", 0, 65535, &p25) || !ls_num(o, "len_p50", 0, 65535, &p50) ||
        !ls_num(o, "len_p90", 0, 65535, &p90) || !ls_num(o, "bubbles_p50", 0, 1000, &bub) ||
        !ls_num(o, "lower_start_rate", 0, 1, &low) || !ls_num(o, "emoji_rate", 0, 1, &emo) ||
        !ls_num(o, "end_punct_rate", 0, 1, &endp))
        return false;
    int32_t lat = -1;
    const hu_json_value_t *lv = hu_json_object_get(o, "latency_p50_s");
    if (lv && lv->type == HU_JSON_NUMBER && lv->data.number >= 0 && lv->data.number < 2.0e9)
        lat = (int32_t)lv->data.number;
    else if (lv && lv->type != HU_JSON_NULL)
        return false;
    memset(out, 0, sizeof(*out));
    out->n = (uint32_t)n;
    out->n_eff = (float)n_eff;
    out->len_p25 = (uint16_t)p25;
    out->len_p50 = (uint16_t)p50;
    out->len_p90 = (uint16_t)p90;
    out->bubbles_p50 = (float)bub;
    out->lower_start_rate = (float)low;
    out->emoji_rate = (float)emo;
    out->end_punct_rate = (float)endp;
    out->latency_p50_s = lat;
    return true;
}

static bool ls_document_valid(const hu_json_value_t *root) {
    const char *schema = hu_json_get_string(root, "schema");
    if (!schema || strcmp(schema, HU_LEARNED_STYLE_SCHEMA) != 0)
        return false;
    hu_learned_style_t g;
    return ls_read_stats(hu_json_object_get(root, "global"), &g);
}

/* Bring the cache in line with the file. Caller holds s_ls_mu. */
static void ls_refresh_locked(void) {
    if (!s_ls.persona[0])
        return;
    char base[HU_LS_PATH_MAX];
    char path[HU_LS_PATH_MAX];
    int n =
        hu_persona_base_dir(base, sizeof(base))
            ? snprintf(path, sizeof(path), "%s/%s%s", base, s_ls.persona, HU_LEARNED_STYLE_SUFFIX)
            : -1;
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        ls_drop_locked();
        return;
    }
    if (strcmp(path, s_ls.path) != 0) {
        ls_drop_locked(); /* persona dir moved (HU_PERSONA_DIR) */
        memcpy(s_ls.path, path, sizeof(path));
    }
    time_t now = ls_now();
    if (s_ls.have_stat && now >= s_ls.last_stat_at &&
        now - s_ls.last_stat_at < HU_LEARNED_STYLE_RESTAT_SECS)
        return;
    s_ls.have_stat = true;
    s_ls.last_stat_at = now;

    struct stat st;
    if (stat(s_ls.path, &st) != 0) {
        if (s_ls.root) {
            hu_allocator_t a = hu_system_allocator();
            hu_json_free(&a, s_ls.root);
            s_ls.root = NULL;
        }
        s_ls.present = false;
        return;
    }
    if (s_ls.present && st.st_mtime == s_ls.mtime && st.st_size == s_ls.size)
        return; /* unchanged since the last load (valid or not) */

    hu_allocator_t a = hu_system_allocator();
    if (s_ls.root) {
        hu_json_free(&a, s_ls.root);
        s_ls.root = NULL;
    }
    s_ls.present = true;
    s_ls.mtime = st.st_mtime;
    s_ls.size = st.st_size;

    char *buf = NULL;
    size_t got = 0;
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_persona_card_slurp(&a, s_ls.persona, strlen(s_ls.persona),
                                           HU_LEARNED_STYLE_SUFFIX, &buf, &got);
    if (err == HU_OK)
        err = hu_persona_card_parse_object(&a, buf, got, &root);
    if (buf)
        a.free(a.ctx, buf, got + 1);
    if (err == HU_OK && root && ls_document_valid(root)) {
        s_ls.root = root;
        return;
    }
    if (root)
        hu_json_free(&a, root);
    /* Once per process: the operator needs to know the learned style is not
     * being applied and why, without a line per turn. */
    static atomic_bool warned_malformed = false;
    if (hu_log_once_check_(&warned_malformed))
        hu_log_warn("learned_style", NULL,
                    "%s%s is malformed or not schema %s (err=%d); treating it as absent — "
                    "re-run the learned-style learner",
                    s_ls.persona, HU_LEARNED_STYLE_SUFFIX, HU_LEARNED_STYLE_SCHEMA, (int)err);
}

bool hu_learned_style_lookup(const char *contact_id, size_t len, hu_ls_shape_t shape,
                             hu_learned_style_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->latency_p50_s = -1;
    pthread_mutex_lock(&s_ls_mu);
    ls_refresh_locked();
    const hu_json_value_t *root = s_ls.root;
    if (root) {
        const hu_json_value_t *contact = NULL;
        char key[HU_LS_KEY_MAX];
        if (contact_id && len > 0 && len < sizeof(key)) {
            memcpy(key, contact_id, len);
            key[len] = '\0';
            const hu_json_value_t *contacts = hu_json_object_get(root, "contacts");
            if (contacts && contacts->type == HU_JSON_OBJECT)
                contact = hu_json_object_get(contacts, key);
        }
        if (contact && contact->type == HU_JSON_OBJECT) {
            char bucket[32];
            snprintf(bucket, sizeof(bucket), "shape:%s", hu_learned_style_shape_name(shape));
            const hu_json_value_t *buckets = hu_json_object_get(contact, "buckets");
            if (buckets && buckets->type == HU_JSON_OBJECT &&
                ls_read_stats(hu_json_object_get(buckets, bucket), out)) {
                out->found = out->from_bucket = out->from_contact = true;
            } else if (ls_read_stats(hu_json_object_get(contact, "overall"), out)) {
                out->found = out->from_contact = true;
            }
        }
        if (!out->found && ls_read_stats(hu_json_object_get(root, "global"), out))
            out->found = true;
    }
    pthread_mutex_unlock(&s_ls_mu);
    return out->found;
}

/* ── render ──────────────────────────────────────────────────────────── */

#define HU_LS_FIRST_NAME_MAX 40

size_t hu_learned_style_render_line(const hu_learned_style_t *ls, hu_ls_shape_t shape,
                                    const char *first_name, size_t first_name_len, char *buf,
                                    size_t cap) {
    if (!ls || !ls->found || !buf || cap == 0)
        return 0;
    /* First token of the persona contact's name; "them" without one. */
    size_t fl = 0;
    if (first_name) {
        while (fl < first_name_len && fl < HU_LS_FIRST_NAME_MAX && first_name[fl] &&
               !isspace((unsigned char)first_name[fl]))
            fl++;
    }
    const char *who = fl ? first_name : "them";
    int wl = fl ? (int)fl : 4;
    const char *when = "";
    if (ls->from_bucket && shape == HU_LS_SHAPE_QUESTION)
        when = " when they ask something";
    else if (ls->from_bucket && shape == HU_LS_SHAPE_STORY)
        when = " when they tell you something big";

    size_t pos = 0;
    int n = snprintf(buf, cap, "How you text %.*s%s: usually about %u characters, up to about %u",
                     wl, who, when, (unsigned)ls->len_p50, (unsigned)ls->len_p90);
    if (n < 0 || (size_t)n >= cap)
        goto too_small;
    pos = (size_t)n;

    const float lo = HU_LEARNED_STYLE_RATE_LOW, hi = HU_LEARNED_STYLE_RATE_HIGH;
    const char *clauses[3];
    size_t nc = 0;
    if (ls->lower_start_rate >= hi)
        clauses[nc++] = "lowercase start most of the time";
    else if (ls->lower_start_rate <= lo)
        clauses[nc++] = "start with a capital letter most of the time";
    if (ls->end_punct_rate >= hi)
        clauses[nc++] = "usually end with punctuation";
    else if (ls->end_punct_rate <= lo)
        clauses[nc++] = "rarely end with punctuation";
    if (ls->emoji_rate >= hi)
        clauses[nc++] = "use an emoji most of the time";
    else if (ls->emoji_rate <= lo)
        clauses[nc++] = "rarely use emoji";
    for (size_t i = 0; i < nc; i++) {
        n = snprintf(buf + pos, cap - pos, "; %s", clauses[i]);
        if (n < 0 || (size_t)n >= cap - pos)
            goto too_small;
        pos += (size_t)n;
    }
    if (pos + 2 > cap)
        goto too_small;
    buf[pos++] = '.';
    buf[pos] = '\0';
    return pos;
too_small:
    buf[0] = '\0';
    return 0;
}

/* ── length-rule classifier ──────────────────────────────────────────── */

static bool ls_word_start(const char *s, size_t i) {
    return i == 0 || !isalnum((unsigned char)s[i - 1]);
}

static size_t ls_skip_digits(const char *s, size_t len, size_t i) {
    while (i < len && isdigit((unsigned char)s[i]))
        i++;
    return i;
}

static size_t ls_skip_spaces(const char *s, size_t len, size_t i) {
    while (i < len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    return i;
}

static bool ls_prefix_ci(const char *s, size_t len, size_t i, const char *p) {
    size_t pl = strlen(p);
    return i + pl <= len && strncasecmp(s + i, p, pl) == 0;
}

/* "5 words", "5-15 words", "20 to 60 characters", "60 chars". */
static bool ls_has_count(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i]) || !ls_word_start(s, i))
            continue;
        size_t j = ls_skip_digits(s, len, i);
        size_t k = ls_skip_spaces(s, len, j);
        if (k < len && s[k] == '-')
            k++;
        else if (ls_prefix_ci(s, len, k, "to "))
            k += 3;
        else
            k = len; /* no range */
        if (k < len) {
            k = ls_skip_spaces(s, len, k);
            if (k < len && isdigit((unsigned char)s[k]))
                j = ls_skip_digits(s, len, k);
        }
        j = ls_skip_spaces(s, len, j);
        if (ls_prefix_ci(s, len, j, "word") || ls_prefix_ci(s, len, j, "char"))
            return true;
        i = j;
    }
    return false;
}

/* "MAX 15", "max: 20". */
static bool ls_has_max_n(const char *s, size_t len) {
    for (size_t i = 0; i + 3 <= len; i++) {
        if (!ls_word_start(s, i) || strncasecmp(s + i, "max", 3) != 0)
            continue;
        size_t j = i + 3;
        if (j < len && isalnum((unsigned char)s[j]))
            continue; /* "maximum" etc. */
        while (j < len && (s[j] == ' ' || s[j] == ':'))
            j++;
        if (j < len && isdigit((unsigned char)s[j]))
            return true;
    }
    return false;
}

bool hu_learned_style_is_length_rule(const char *s, size_t len) {
    if (!s || len == 0)
        return false;
    if (ls_has_count(s, len) || ls_has_max_n(s, len) ||
        hu_str_contains_word_ci_n(s, len, "one line") ||
        hu_str_contains_word_ci_n(s, len, "one-line"))
        return true;
    static const char *const brevity[] = {"brief", "briefly", "short", "shorter"};
    for (size_t i = 0; i < sizeof(brevity) / sizeof(brevity[0]); i++) {
        if (hu_str_contains_word_ci_n(s, len, brevity[i]))
            /* "Match the energy… short message gets short reply" mirrors the
             * other person; it is relative, not a fixed length, and stays. */
            return !hu_str_contains_word_ci_n(s, len, "match");
    }
    return false;
}

bool hu_persona_style_opts_suppress(hu_persona_style_opts_t *opts, const char *entry) {
    if (!opts || !opts->suppress_length_rules || !entry ||
        !hu_learned_style_is_length_rule(entry, strlen(entry)))
        return false;
    opts->suppressed++;
    return true;
}

/* ── contact profile stripping ───────────────────────────────────────── */

static void ls_emit(char *out, size_t cap, size_t *pos, const char *s, size_t n) {
    if (out && *pos + n < cap)
        memcpy(out + *pos, s, n);
    *pos += n;
}

/* Strip length sentences from one "Dynamic: …" body. Returns removed count;
 * writes the kept body (trailing spaces trimmed) through ls_emit. */
static size_t ls_strip_sentences(const char *b, size_t blen, char *out, size_t cap, size_t *pos,
                                 size_t *kept) {
    size_t removed = 0, i = 0, kept_bytes = 0;
    size_t start_pos = *pos;
    while (i < blen) {
        size_t s0 = i;
        while (i < blen && b[i] != '.' && b[i] != '!' && b[i] != '?')
            i++;
        while (i < blen && (b[i] == '.' || b[i] == '!' || b[i] == '?'))
            i++;
        while (i < blen && b[i] == ' ')
            i++;
        if (hu_learned_style_is_length_rule(b + s0, i - s0)) {
            removed++;
            continue;
        }
        ls_emit(out, cap, pos, b + s0, i - s0);
        kept_bytes += i - s0;
    }
    /* Trim the space left behind when the LAST sentence was dropped. The
     * caller guarantees cap >= input length + 1, so every emit landed. */
    if (out && removed > 0) {
        while (*pos > start_pos && out[*pos - 1] == ' ')
            (*pos)--;
        kept_bytes = *pos - start_pos;
    }
    *kept = kept_bytes;
    return removed;
}

size_t hu_learned_style_strip_contact(const char *in, size_t len, char *out, size_t cap,
                                      size_t *out_len) {
    static const char k_dyn[] = "Dynamic: ";
    static const char k_pat[] = "Pattern: ";
    if (out_len)
        *out_len = 0;
    if (!in)
        return 0;
    if (out && cap < len + 1)
        return 0;
    size_t pos = 0, removed = 0, i = 0;
    while (i < len) {
        size_t ls0 = i;
        while (i < len && in[i] != '\n')
            i++;
        size_t le = i; /* line end, newline excluded */
        size_t nl = (i < len) ? 1 : 0;
        i += nl;
        size_t llen = le - ls0;
        if (llen >= sizeof(k_dyn) - 1 && memcmp(in + ls0, k_dyn, sizeof(k_dyn) - 1) == 0) {
            size_t line_pos = pos, kept = 0;
            ls_emit(out, cap, &pos, k_dyn, sizeof(k_dyn) - 1);
            size_t body = ls0 + sizeof(k_dyn) - 1;
            size_t r = ls_strip_sentences(in + body, le - body, out, cap, &pos, &kept);
            removed += r;
            if (r > 0 && kept == 0) {
                pos = line_pos; /* nothing left: the line goes */
                continue;
            }
            ls_emit(out, cap, &pos, "\n", nl);
            continue;
        }
        if (llen >= sizeof(k_pat) - 1 && memcmp(in + ls0, k_pat, sizeof(k_pat) - 1) == 0 &&
            hu_learned_style_is_length_rule(in + ls0, llen)) {
            removed++;
            continue;
        }
        ls_emit(out, cap, &pos, in + ls0, llen + nl);
    }
    if (out) {
        out[pos] = '\0';
        if (out_len)
            *out_len = pos;
    }
    return removed;
}
