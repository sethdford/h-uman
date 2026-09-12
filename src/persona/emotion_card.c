/* Emotion card — see include/human/persona/emotion_card.h for the why.
 *
 * Reads ~/.human/personas/<persona>.emotion-card.json (emotion-card/v1,
 * written by scripts/measure_emotion_card.py) and renders casual rule 14
 * from it. There is deliberately no compiled default: a missing card is
 * "not measured", logged once, and the rule is simply not rendered. */
#include "human/persona/emotion_card.h"

#include "human/core/json.h"
#include "human/core/log.h"
#include "human/persona/card_file.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

void hu_emotion_card_clear(hu_emotion_card_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
}

/* <key>.value within [lo, hi]; false when absent, NaN or out of range. */
static bool read_axis(const hu_json_value_t *root, const char *key, double lo, double hi,
                      double *out) {
    const hu_json_value_t *axis = hu_json_object_get(root, key);
    if (!axis || axis->type != HU_JSON_OBJECT)
        return false;
    double v = hu_json_get_number(axis, "value", NAN);
    if (!(v >= lo && v <= hi)) /* also rejects NaN */
        return false;
    *out = v;
    return true;
}

static void read_top(const hu_json_value_t *root, hu_emotion_card_t *card) {
    const hu_json_value_t *top = hu_json_object_get(root, "top");
    if (!top || top->type != HU_JSON_ARRAY)
        return;
    for (size_t i = 0; i < top->data.array.len && card->top_count < HU_EMOTION_CARD_TOP_MAX; i++) {
        const hu_json_value_t *item = top->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        const char *name = hu_json_get_string(item, "emotion");
        double share = hu_json_get_number(item, "share", NAN);
        if (!name || !*name || strlen(name) >= HU_EMOTION_CARD_NAME_MAX ||
            !(share >= 0.0 && share <= 1.0))
            continue;
        hu_emotion_card_top_t *t = &card->top[card->top_count++];
        snprintf(t->emotion, sizeof(t->emotion), "%s", name);
        t->share = share;
    }
}

hu_error_t hu_emotion_card_parse(hu_allocator_t *alloc, const char *json, size_t len,
                                 hu_emotion_card_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_emotion_card_clear(out);
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_persona_card_parse_object(alloc, json, len, &root);
    if (err != HU_OK)
        return err;

    hu_emotion_card_t card;
    hu_emotion_card_clear(&card);
    double n = hu_json_get_number(root, "n", 0.0);
    bool ok = n >= 1.0 && read_axis(root, "neutral_share", 0.0, 1.0, &card.neutral_share) &&
              read_axis(root, "mean_intensity", 0.0, 1.0, &card.mean_intensity) &&
              read_axis(root, "valence_mean", -1.0, 1.0, &card.valence_mean);
    if (ok) {
        card.n = (unsigned)n;
        read_top(root, &card);
        hu_persona_card_copy_window(root, card.window_start, sizeof(card.window_start),
                                    card.window_end, sizeof(card.window_end));
        card.from_card = true;
        *out = card;
    }
    hu_json_free(alloc, root);
    return ok ? HU_OK : HU_ERR_INVALID_ARGUMENT;
}

hu_error_t hu_emotion_card_load_for_persona(hu_allocator_t *alloc, const char *name,
                                            size_t name_len, hu_emotion_card_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_emotion_card_clear(out);
    char *buf = NULL;
    size_t got = 0;
    hu_error_t err = hu_persona_card_slurp(alloc, name, name_len, ".emotion-card.json", &buf, &got);
    if (err != HU_OK)
        return err;
    err = hu_emotion_card_parse(alloc, buf, got, out);
    alloc->free(alloc->ctx, buf, got + 1);
    return err;
}

bool hu_emotion_card_resolve(const char *name, size_t name_len, hu_emotion_card_t *out) {
    if (!out)
        return false;
    hu_emotion_card_clear(out);
    if (!name || name_len == 0)
        return false;
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_emotion_card_load_for_persona(&alloc, name, name_len, out);
    if (err == HU_OK)
        return true;
    /* Once per process either way: "not measured" must be a visible fact,
     * and an unreadable card is a bug to fix, not a silent fallback. */
    if (err == HU_ERR_NOT_FOUND) {
        static atomic_bool noted_missing = false;
        hu_log_info_once(&noted_missing, "persona", NULL,
                         "no emotion card for persona '%.*s' — emotional register is not "
                         "measured; run scripts/measure_emotion_card.py --persona %.*s to "
                         "write %.*s.emotion-card.json",
                         (int)name_len, name, (int)name_len, name, (int)name_len, name);
    } else {
        static atomic_bool warned_unreadable = false;
        hu_log_warn_once(&warned_unreadable, "persona", NULL,
                         "emotion card for persona '%.*s' unreadable (err=%d); ignoring it",
                         (int)name_len, name, (int)err);
    }
    hu_emotion_card_clear(out);
    return false;
}

/* "amusement, interest and satisfaction" from the top entries (max 3);
 * "rare" when the card has none. */
static void fmt_top(const hu_emotion_card_t *card, char *out, size_t cap) {
    unsigned n = card->top_count < 3 ? card->top_count : 3;
    if (n == 0) {
        snprintf(out, cap, "rare");
        return;
    }
    size_t used = 0;
    for (unsigned i = 0; i < n && used < cap; i++) {
        const char *sep = i == 0 ? "" : (i + 1 == n ? " and " : ", ");
        int w = snprintf(out + used, cap - used, "%s%s", sep, card->top[i].emotion);
        if (w < 0)
            break;
        used += (size_t)w;
    }
}

hu_error_t hu_emotion_card_render_rule(const hu_emotion_card_t *card, char *buf, size_t cap,
                                       size_t *out_len) {
    if (!card || !buf || cap == 0 || !card->from_card)
        return HU_ERR_INVALID_ARGUMENT;
    char top[3 * HU_EMOTION_CARD_NAME_MAX + 16];
    fmt_top(card, top, sizeof(top));
    int n = snprintf(buf, cap,
                     "14. Emotional register is MEASURED (%u of your texts): about %d%% read "
                     "as neutral or matter-of-fact; when feeling shows it is mostly %s, and "
                     "it stays low-key (about %d out of 10). Match that. Don't perform "
                     "feelings you wouldn't actually text.\n",
                     card->n, (int)lround(card->neutral_share * 100.0), top,
                     (int)lround(card->mean_intensity * 10.0));
    if (n < 0 || (size_t)n + 1 > cap)
        return HU_ERR_OUT_OF_MEMORY;
    if (out_len)
        *out_len = (size_t)n;
    return HU_OK;
}

hu_gate_mode_t hu_emotion_register_mode(void) {
    return hu_gate_mode_from_env("HU_EMOTION_REGISTER", HU_GATE_OFF);
}
