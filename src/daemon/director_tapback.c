/* What Seth actually does, for director v2. Contract, field list and the
 * #586 reconciliation note: include/human/daemon/director_tapback.h. The
 * profile file holds numbers only, so nothing read here can put message text
 * into a prompt or a log; reaction names come only from hu_tapback_kind_names. */
#include "human/daemon/director_tapback.h"
#include "human/core/log.h"
#include "human/persona.h"
#include "human/persona/card_file.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ── inbound shape (mirrors #586 hu_learned_style_shape_inbound) ───── */

static const char *const k_injected_prefixes[] = {
    "[Audio transcription: ",
    "[Video transcription: ",
    "[They sent a photo: ",
    "[They sent a video]",
    "[They sent a picture ",
    "[Photo]",
    "[Video]",
    "[Audio]",
};

static bool injected_note(const char *s, size_t n) {
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    if (n == 0 || s[0] != '[' || s[n - 1] != ']')
        return false;
    for (size_t i = 0; i < sizeof(k_injected_prefixes) / sizeof(k_injected_prefixes[0]); i++) {
        size_t pl = strlen(k_injected_prefixes[i]);
        if (n >= pl && memcmp(s, k_injected_prefixes[i], pl) == 0)
            return true;
    }
    return false;
}

static hu_dir_shape_t shape_of_text(const char *s, size_t len) {
    size_t a = 0, b = len;
    while (a < b && isspace((unsigned char)s[a]))
        a++;
    while (b > a && isspace((unsigned char)s[b - 1]))
        b--;
    size_t n = b - a;
    if (n == 0)
        return HU_DIR_SHAPE_CASUAL;
    if (memchr(s + a, '?', n))
        return HU_DIR_SHAPE_QUESTION;
    if (n >= 140)
        return HU_DIR_SHAPE_STORY;
    if (n < 80)
        return HU_DIR_SHAPE_CASUAL;
    size_t runs = 0;
    for (size_t i = a; i < b; i++) {
        bool term = s[i] == '.' || s[i] == '!';
        bool prev = i > a && (s[i - 1] == '.' || s[i - 1] == '!');
        if (term && !prev)
            runs++;
    }
    return runs >= 2 ? HU_DIR_SHAPE_STORY : HU_DIR_SHAPE_CASUAL;
}

hu_dir_shape_t hu_director_inbound_shape(const char *batch, size_t len) {
    if (!batch || len == 0)
        return HU_DIR_SHAPE_CASUAL;
    hu_allocator_t a = hu_system_allocator();
    char *buf = (char *)a.alloc(a.ctx, len + 1);
    if (!buf)
        return shape_of_text(batch, len);
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        size_t start = i;
        while (i < len && batch[i] != '\n')
            i++;
        size_t ll = i - start;
        if (i < len)
            i++;
        if (injected_note(batch + start, ll))
            continue;
        if (o > 0)
            buf[o++] = '\n';
        memcpy(buf + o, batch + start, ll);
        o += ll;
    }
    hu_dir_shape_t shape = shape_of_text(buf, o);
    a.free(a.ctx, buf, len + 1);
    return shape;
}

const char *hu_director_shape_name(hu_dir_shape_t shape) {
    return shape == HU_DIR_SHAPE_QUESTION ? "question"
           : shape == HU_DIR_SHAPE_STORY  ? "story"
                                          : "casual";
}

const char *hu_tapback_src_name(hu_tapback_src_t src) {
    return src == HU_TAPBACK_SRC_LEARNED ? "learned"
           : src == HU_TAPBACK_SRC_MODEL ? "model"
                                         : "nodata";
}

const char *const hu_tapback_kind_names[HU_TAPBACK_KINDS] = {
    "heart", "haha", "thumbs_up", "emphasis", "thumbs_down", "question",
};

/* ── reading the profile ───────────────────────────────────────────── */

static bool rate_field(const hu_json_value_t *o, const char *key, float *out) {
    const hu_json_value_t *v = o && o->type == HU_JSON_OBJECT ? hu_json_object_get(o, key) : NULL;
    if (!v || v->type != HU_JSON_NUMBER || !(v->data.number >= 0.0 && v->data.number <= 1.0))
        return false; /* also rejects NaN */
    *out = (float)v->data.number;
    return true;
}

static bool count_field(const hu_json_value_t *o, const char *key, uint32_t *out) {
    const hu_json_value_t *v = o && o->type == HU_JSON_OBJECT ? hu_json_object_get(o, key) : NULL;
    if (!v || v->type != HU_JSON_NUMBER || !(v->data.number >= 0 && v->data.number < 4.0e9))
        return false;
    *out = (uint32_t)v->data.number;
    return true;
}

static bool types_field(const hu_json_value_t *o, float types[HU_TAPBACK_KINDS]) {
    const hu_json_value_t *t =
        o && o->type == HU_JSON_OBJECT ? hu_json_object_get(o, "tapback_types") : NULL;
    if (!t || t->type != HU_JSON_OBJECT)
        return false;
    bool any = false;
    for (size_t k = 0; k < HU_TAPBACK_KINDS; k++) {
        types[k] = 0.0f;
        if (rate_field(t, hu_tapback_kind_names[k], &types[k]))
            any = true;
    }
    return any;
}

/* A cell's tapback-only rate, counted only with the learner's minimum n. */
static bool cell_rate(const hu_json_value_t *o, uint32_t min_n, float *rate, uint32_t *n) {
    return count_field(o, "n", n) && *n >= min_n && rate_field(o, "tapback_only_rate", rate);
}

typedef struct {
    float rate;
    uint32_t n;
} tb_cell_t;

#define HU_TAPBACK_MAX_CELLS 1024u

/* n-weighted lower quartile: the smallest rate whose cumulative weight
 * reaches a quarter of the total. */
static float weighted_lower_quartile(tb_cell_t *c, size_t k) {
    double total = 0;
    for (size_t i = 1; i < k; i++) { /* small k: one cell per contact (x shape) */
        tb_cell_t x = c[i];
        size_t j = i;
        while (j > 0 && c[j - 1].rate > x.rate) {
            c[j] = c[j - 1];
            j--;
        }
        c[j] = x;
    }
    for (size_t i = 0; i < k; i++)
        total += c[i].n;
    double cum = 0;
    for (size_t i = 0; i < k; i++) {
        cum += c[i].n;
        if (cum >= 0.25 * total)
            return c[i].rate;
    }
    return c[k - 1].rate;
}

/* Peer cells at the lookup's level: every contact's shape buckets (bucket
 * lookup) or every contact's overall (contact lookup). */
static void derive_cutoff(const hu_json_value_t *contacts, bool buckets_level,
                          hu_tapback_profile_t *out) {
    tb_cell_t cells[HU_TAPBACK_MAX_CELLS];
    size_t k = 0;
    for (size_t c = 0; contacts && c < contacts->data.object.len; c++) {
        const hu_json_value_t *ct = contacts->data.object.pairs[c].value;
        if (!ct || ct->type != HU_JSON_OBJECT)
            continue;
        if (!buckets_level) {
            if (k < HU_TAPBACK_MAX_CELLS &&
                cell_rate(hu_json_object_get(ct, "overall"), HU_TAPBACK_MIN_CONTACT_N,
                          &cells[k].rate, &cells[k].n))
                k++;
            continue;
        }
        const hu_json_value_t *b = hu_json_object_get(ct, "buckets");
        for (size_t i = 0; b && b->type == HU_JSON_OBJECT && i < b->data.object.len; i++) {
            const hu_json_pair_t *pr = &b->data.object.pairs[i];
            if (k < HU_TAPBACK_MAX_CELLS && pr->key_len > 6 && memcmp(pr->key, "shape:", 6) == 0 &&
                cell_rate(pr->value, HU_TAPBACK_MIN_BUCKET_N, &cells[k].rate, &cells[k].n))
                k++;
        }
    }
    out->cells = (uint32_t)k;
    if (k >= HU_TAPBACK_MIN_CELLS) {
        out->cutoff = weighted_lower_quartile(cells, k);
        out->cutoff_found = true;
    }
}

bool hu_tapback_profile_from_json(const hu_json_value_t *root, const char *contact,
                                  size_t contact_len, hu_dir_shape_t shape,
                                  hu_tapback_profile_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->level = "none";
    if (!root || root->type != HU_JSON_OBJECT)
        return false;
    const char *schema = hu_json_get_string(root, "schema");
    if (!schema || strcmp(schema, "learned-style/v1") != 0)
        return false;

    const hu_json_value_t *contacts = hu_json_object_get(root, "contacts");
    if (contacts && contacts->type != HU_JSON_OBJECT)
        contacts = NULL;
    const hu_json_value_t *me = NULL;
    char key[160];
    if (contacts && contact && contact_len > 0 && contact_len < sizeof(key)) {
        memcpy(key, contact, contact_len);
        key[contact_len] = '\0';
        me = hu_json_object_get(contacts, key);
        if (me && me->type != HU_JSON_OBJECT)
            me = NULL;
    }
    char bucket_key[32];
    (void)snprintf(bucket_key, sizeof(bucket_key), "shape:%s", hu_director_shape_name(shape));
    const hu_json_value_t *buckets = me ? hu_json_object_get(me, "buckets") : NULL;
    const hu_json_value_t *levels[3] = {
        buckets && buckets->type == HU_JSON_OBJECT ? hu_json_object_get(buckets, bucket_key) : NULL,
        me ? hu_json_object_get(me, "overall") : NULL,
        hu_json_object_get(root, "global"),
    };
    static const char *const names[3] = {"bucket", "contact", "global"};
    const uint32_t min_n[3] = {HU_TAPBACK_MIN_BUCKET_N, HU_TAPBACK_MIN_CONTACT_N, 0};

    for (size_t l = 0; l < 3; l++) {
        if (!out->found && cell_rate(levels[l], min_n[l], &out->rate, &out->n)) {
            out->found = true;
            out->level = names[l];
        }
        if (!out->types_found)
            out->types_found = types_field(levels[l], out->types);
        if (!out->disengage_found &&
            rate_field(levels[l], "tapback_disengage_rate", &out->disengage_rate)) {
            out->disengage_found = true;
            (void)count_field(levels[l], "tapback_disengage_n", &out->disengage_n);
        }
        static const char *const form_keys[HU_FORM_KINDS] = {"voice_memo_rate", "gif_rate",
                                                             "share_rate"};
        for (size_t f = 0; f < HU_FORM_KINDS; f++)
            if (!(out->form_found & (1u << f)) &&
                rate_field(levels[l], form_keys[f], &out->form_rates[f]))
                out->form_found |= 1u << f;
        uint32_t lat = 0;
        if (!out->latency_found && count_field(levels[l], "latency_p50_s", &lat)) {
            out->latency_found = true;
            out->latency_s = (int32_t)lat;
        }
    }
    if (out->found && strcmp(out->level, "global") != 0)
        derive_cutoff(contacts, strcmp(out->level, "bucket") == 0, out);
    return out->found || out->types_found || out->disengage_found || out->latency_found ||
           out->form_found != 0;
}

/* ── the mtime cache ───────────────────────────────────────────────── */

static pthread_mutex_t s_tb_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char path[512];
    time_t mtime;
    off_t size;
    hu_json_value_t *root;
} s_tb;

static void cache_drop_locked(void) {
    if (s_tb.root) {
        hu_allocator_t a = hu_system_allocator();
        hu_json_free(&a, s_tb.root);
    }
    memset(&s_tb, 0, sizeof(s_tb));
}

void hu_tapback_profile_cache_reset(void) {
    pthread_mutex_lock(&s_tb_mu);
    cache_drop_locked();
    pthread_mutex_unlock(&s_tb_mu);
}

/* Bring the cache in line with the file. Caller holds s_tb_mu. */
static const hu_json_value_t *cache_root_locked(const char *persona, size_t persona_len) {
    char base[400];
    char path[512];
    int n = hu_persona_base_dir(base, sizeof(base))
                ? snprintf(path, sizeof(path), "%s/%.*s.learned-style.json", base, (int)persona_len,
                           persona)
                : -1;
    struct stat st;
    if (n <= 0 || (size_t)n >= sizeof(path) || stat(path, &st) != 0) {
        cache_drop_locked();
        return NULL;
    }
    if (s_tb.root && strcmp(s_tb.path, path) == 0 && s_tb.mtime == st.st_mtime &&
        s_tb.size == st.st_size)
        return s_tb.root;
    cache_drop_locked();
    hu_allocator_t a = hu_system_allocator();
    char *buf = NULL;
    size_t got = 0;
    hu_json_value_t *root = NULL;
    hu_error_t err =
        hu_persona_card_slurp(&a, persona, persona_len, ".learned-style.json", &buf, &got);
    if (err == HU_OK)
        err = hu_persona_card_parse_object(&a, buf, got, &root);
    if (buf)
        a.free(a.ctx, buf, got + 1);
    if (err != HU_OK || !root)
        return NULL;
    memcpy(s_tb.path, path, (size_t)n + 1);
    s_tb.mtime = st.st_mtime;
    s_tb.size = st.st_size;
    s_tb.root = root;
    return root;
}

bool hu_tapback_profile_load(const char *persona, size_t persona_len, const char *contact,
                             size_t contact_len, hu_dir_shape_t shape, hu_tapback_profile_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->level = "none";
    if (!persona || persona_len == 0)
        return false;
    pthread_mutex_lock(&s_tb_mu);
    const hu_json_value_t *root = cache_root_locked(persona, persona_len);
    bool found = root && hu_tapback_profile_from_json(root, contact, contact_len, shape, out);
    pthread_mutex_unlock(&s_tb_mu);
    return found;
}

/* ── the prompt facts and the post-check ───────────────────────────── */

static int pct(float r) {
    return (int)(r * 100.0f + 0.5f);
}

size_t hu_tapback_profile_facts(const hu_tapback_profile_t *p, hu_dir_shape_t shape, char *buf,
                                size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    bool forms = p && p->form_found != 0;
    if (!p || (!p->found && !p->types_found && !p->disengage_found && !p->latency_found && !forms))
        return 0;
    size_t pos = 0;
#define FACT(...)                                             \
    do {                                                      \
        int fw = snprintf(buf + pos, cap - pos, __VA_ARGS__); \
        if (fw < 0 || pos + (size_t)fw >= cap)                \
            goto too_small;                                   \
        pos += (size_t)fw;                                    \
    } while (0)
    FACT("How Seth replies, measured from his own texts:");
    if (p->latency_found) {
        int32_t s = p->latency_s;
        if (s < 90)
            FACT(" he usually answers them after about %d seconds;", (int)s);
        else if (s < 90 * 60)
            FACT(" he usually answers them after about %d minutes;", (int)((s + 30) / 60));
        else
            FACT(" he usually answers them after about %d hours;", (int)((s + 1800) / 3600));
    }
    if (p->found) {
        const char *when = strcmp(p->level, "global") == 0    ? "with everyone, overall"
                           : strcmp(p->level, "contact") == 0 ? "with them, overall"
                           : shape == HU_DIR_SHAPE_QUESTION   ? "with them, when they ask something"
                           : shape == HU_DIR_SHAPE_STORY ? "with them, when they tell him something"
                                                         : "with them, on casual messages";
        FACT(" %s he replies with only a reaction %d%% of the time (n=%u);", when, pct(p->rate),
             (unsigned)p->n);
    }
    if (p->types_found) {
        FACT(" his reactions:");
        bool first = true;
        for (size_t k = 0; k < HU_TAPBACK_KINDS; k++)
            if (p->types[k] > 0.0f) {
                FACT("%s %s %d%%", first ? "" : ",", hu_tapback_kind_names[k], pct(p->types[k]));
                first = false;
            }
        FACT(";");
    }
    if (p->disengage_found) {
        FACT(" after his reaction-only replies they went quiet or pushed for a real answer %d%% "
             "of the time",
             pct(p->disengage_rate));
        if (p->disengage_n > 0)
            FACT(" (n=%u)", (unsigned)p->disengage_n);
        FACT(";");
    }
    if (forms) {
        static const char *const form_names[HU_FORM_KINDS] = {"a voice memo", "a GIF",
                                                              "a shared song or video"};
        bool first = true;
        for (size_t f = 0; f < HU_FORM_KINDS; f++)
            if (p->form_found & (1u << f)) {
                FACT("%s %s %d%%", first ? " of his replies to them:" : ",", form_names[f],
                     pct(p->form_rates[f]));
                first = false;
            }
        FACT(";");
    }
#undef FACT
    buf[pos - 1] = '.'; /* the last ';' */
    return pos;
too_small:
    buf[0] = '\0';
    return 0;
}

hu_tapback_src_t hu_director_v2_tapback_check(hu_director_result_t *result,
                                              const hu_tapback_profile_t *p) {
    if (!p || !p->found || !p->cutoff_found)
        return HU_TAPBACK_SRC_NODATA; /* trust the model */
    if (!result || result->action != DIR_TAPBACK || p->rate > p->cutoff)
        return HU_TAPBACK_SRC_MODEL;
    /* His own data says he almost never answers this contact and shape with
     * only a reaction. Text instead; the model's delay and direction stand. */
    result->action = DIR_TEXT;
    result->form = HU_DIR_FORM_TEXT;
    result->reaction = HU_REACTION_NONE;
    return HU_TAPBACK_SRC_LEARNED;
}
