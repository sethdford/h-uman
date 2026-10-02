/* Learned tapback evidence for director v2. Contract and the reconciliation
 * note with #586: include/human/daemon/director_tapback.h. The file read here
 * holds numbers only, so nothing from it can put message text in a prompt or
 * a log; reaction names are rendered only from hu_tapback_kind_names. */
#include "human/daemon/director_tapback.h"
#include "human/core/log.h"
#include "human/persona/card_file.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── inbound shape ─────────────────────────────────────────────────── */

/* A line the daemon added to the batch ("[They sent a photo: ...]",
 * "[Audio transcription: ...]"): bracketed from end to end. */
static bool injected_note(const char *s, size_t n) {
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    while (n > 0 && isspace((unsigned char)*s)) {
        s++;
        n--;
    }
    return n >= 2 && s[0] == '[' && s[n - 1] == ']';
}

hu_dir_shape_t hu_director_inbound_shape(const char *batch, size_t len) {
    if (!batch || len == 0)
        return HU_DIR_SHAPE_CASUAL;
    /* What they typed: the kept lines re-joined with '\n'. Past the buffer
     * the text is a story by length alone, so only '?' is still scanned. */
    char kept[4096];
    size_t k = 0;
    bool question = false;
    for (size_t i = 0; i < len;) {
        size_t e = i;
        while (e < len && batch[e] != '\n')
            e++;
        if (!injected_note(batch + i, e - i)) {
            if (k > 0 && k < sizeof(kept))
                kept[k++] = '\n';
            for (size_t c = i; c < e; c++) {
                question |= batch[c] == '?';
                if (k < sizeof(kept))
                    kept[k++] = batch[c];
            }
        }
        i = e + 1;
    }
    if (question)
        return HU_DIR_SHAPE_QUESTION;
    size_t a = 0, b = k;
    while (a < b && isspace((unsigned char)kept[a]))
        a++;
    while (b > a && isspace((unsigned char)kept[b - 1]))
        b--;
    size_t n = b - a;
    if (n >= 140)
        return HU_DIR_SHAPE_STORY;
    size_t runs = 0;
    for (size_t c = a; n >= 80 && c < b; c++) {
        bool term = kept[c] == '.' || kept[c] == '!';
        if (term && (c == a || (kept[c - 1] != '.' && kept[c - 1] != '!')))
            runs++;
    }
    return runs >= 2 ? HU_DIR_SHAPE_STORY : HU_DIR_SHAPE_CASUAL;
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

static uint32_t count_field(const hu_json_value_t *o, const char *key) {
    const hu_json_value_t *v = o && o->type == HU_JSON_OBJECT ? hu_json_object_get(o, key) : NULL;
    return v && v->type == HU_JSON_NUMBER && v->data.number >= 0 && v->data.number < 4.0e9
               ? (uint32_t)v->data.number
               : 0;
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

/* Lower quartile, nearest rank, of v[0..n) (sorted in place). */
static float lower_quartile(float *v, size_t n) {
    for (size_t i = 1; i < n; i++) { /* n is small: one cell per contact x shape */
        float x = v[i];
        size_t j = i;
        while (j > 0 && v[j - 1] > x) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
    size_t rank = (n + 3) / 4; /* ceil(0.25 * n) */
    return v[rank - 1];
}

#define HU_TAPBACK_MAX_CELLS 1024u

/* His own distribution of tapback_only_rate over every contact cell. */
static void derive_cutoff(const hu_json_value_t *contacts, hu_tapback_profile_t *out) {
    float cells[HU_TAPBACK_MAX_CELLS];
    size_t n = 0;
    for (size_t c = 0; contacts && c < contacts->data.object.len; c++) {
        const hu_json_value_t *contact = contacts->data.object.pairs[c].value;
        if (!contact || contact->type != HU_JSON_OBJECT)
            continue;
        if (n < HU_TAPBACK_MAX_CELLS &&
            rate_field(hu_json_object_get(contact, "overall"), "tapback_only_rate", &cells[n]))
            n++;
        const hu_json_value_t *b = hu_json_object_get(contact, "buckets");
        for (size_t k = 0; b && b->type == HU_JSON_OBJECT && k < b->data.object.len; k++) {
            const hu_json_pair_t *pr = &b->data.object.pairs[k];
            if (n < HU_TAPBACK_MAX_CELLS && pr->key_len > 6 && memcmp(pr->key, "shape:", 6) == 0 &&
                rate_field(pr->value, "tapback_only_rate", &cells[n]))
                n++;
        }
    }
    out->cells = (uint32_t)n;
    if (n >= HU_TAPBACK_MIN_CELLS) {
        out->cutoff = lower_quartile(cells, n);
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

    for (size_t l = 0; l < 3; l++) {
        if (!out->found && rate_field(levels[l], "tapback_only_rate", &out->rate)) {
            out->found = true;
            out->level = names[l];
            out->n = count_field(levels[l], "n");
        }
        if (!out->types_found)
            out->types_found = types_field(levels[l], out->types);
        if (!out->disengage_found &&
            rate_field(levels[l], "tapback_disengage_rate", &out->disengage_rate)) {
            out->disengage_found = true;
            out->disengage_n = count_field(levels[l], "tapback_disengage_n");
        }
    }
    derive_cutoff(contacts, out);
    return out->found;
}

bool hu_tapback_profile_load(hu_allocator_t *alloc, const char *persona, size_t persona_len,
                             const char *contact, size_t contact_len, hu_dir_shape_t shape,
                             hu_tapback_profile_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
        out->level = "none";
    }
    if (!alloc || !persona || persona_len == 0 || !out)
        return false;
    char *buf = NULL;
    size_t got = 0;
    hu_json_value_t *root = NULL;
    hu_error_t err =
        hu_persona_card_slurp(alloc, persona, persona_len, ".learned-style.json", &buf, &got);
    if (err == HU_OK)
        err = hu_persona_card_parse_object(alloc, buf, got, &root);
    if (buf)
        alloc->free(alloc->ctx, buf, got + 1);
    bool found = err == HU_OK && root &&
                 hu_tapback_profile_from_json(root, contact, contact_len, shape, out);
    if (root)
        hu_json_free(alloc, root);
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
    if (!p || (!p->found && !p->types_found && !p->disengage_found))
        return 0;
    size_t pos = 0;
#define FACT(...)                                             \
    do {                                                      \
        int fw = snprintf(buf + pos, cap - pos, __VA_ARGS__); \
        if (fw < 0 || pos + (size_t)fw >= cap)                \
            goto too_small;                                   \
        pos += (size_t)fw;                                    \
    } while (0)
    FACT("How Seth reacts, measured from his own texts:");
    if (p->found) {
        const char *when = strcmp(p->level, "global") == 0    ? "with everyone, overall"
                           : strcmp(p->level, "contact") == 0 ? "with them, overall"
                           : shape == HU_DIR_SHAPE_QUESTION   ? "with them, when they ask something"
                           : shape == HU_DIR_SHAPE_STORY ? "with them, when they tell him something"
                                                         : "with them, on casual messages";
        FACT(" %s he replies with only a reaction %d%% of the time", when, pct(p->rate));
        if (p->n > 0)
            FACT(" (n=%u)", (unsigned)p->n);
        FACT(";");
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
    /* Seth almost never answers this contact and shape with only a reaction:
     * at or below the lower quartile of his own tapback-only rates. */
    result->action = DIR_TEXT;
    result->form = HU_DIR_FORM_TEXT;
    result->reaction = HU_REACTION_NONE;
    if (result->delay_s == 0)
        result->delay_s = 3;
    if (result->direction[0] == '\0')
        (void)snprintf(result->direction, sizeof(result->direction), "%s",
                       "They said something he would answer ; engage with what they said ; "
                       "nothing");
    return HU_TAPBACK_SRC_LEARNED;
}
