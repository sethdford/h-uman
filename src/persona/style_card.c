/* Style card — see include/human/persona/style_card.h for the why.
 *
 * Reads ~/.human/personas/<persona>.style-card.json (style-card/v2, written
 * by scripts/measure_style_card.py) and renders the casual register's
 * rule 2 from it. The compiled default is a fallback for a missing card and
 * logs once when it is used, so "the prompt is running on stale numbers"
 * is a fact in the service log rather than a mystery. */
#include "human/persona/style_card.h"

#include "human/core/json.h"
#include "human/core/log.h"
#include "human/persona.h"
#include "human/persona/card_file.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Last measurement taken when this default was edited. scripts/
 * measure_style_card.py, 2026-09-03, window 2026-07-05..2026-09-03,
 * n=977 outbound texts:
 *   lowercase_start   0.086 [0.069, 0.104]
 *   no_terminal_punct 0.817 [0.792, 0.840]
 *   question          0.099 [0.081, 0.119]
 *   exclamation       0.039 [0.028, 0.051]
 *   emoji             0.126 [0.105, 0.146]
 * Do NOT tune these by hand: re-run the script and let the card win. */
void hu_style_card_default(hu_style_card_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->lowercase_start_rate = 0.086;
    out->no_terminal_punct_rate = 0.817;
    out->question_rate = 0.099;
    out->exclamation_rate = 0.039;
    out->emoji_rate = 0.126;
    out->n = 0;
    out->from_card = false;
}

/* Read axes.<name>.value; false when absent or outside [0, 1]. */
static bool read_axis(const hu_json_value_t *axes, const char *name, double *out) {
    const hu_json_value_t *axis = hu_json_object_get(axes, name);
    if (!axis || axis->type != HU_JSON_OBJECT)
        return false;
    double v = hu_json_get_number(axis, "value", -1.0);
    if (!(v >= 0.0 && v <= 1.0)) /* also rejects NaN */
        return false;
    *out = v;
    return true;
}

hu_error_t hu_style_card_parse(hu_allocator_t *alloc, const char *json, size_t len,
                               hu_style_card_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_style_card_default(out);
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_persona_card_parse_object(alloc, json, len, &root);
    if (err != HU_OK)
        return err;

    hu_style_card_t card;
    hu_style_card_default(&card);
    err = HU_ERR_INVALID_ARGUMENT;
    const hu_json_value_t *axes = hu_json_object_get(root, "axes");
    if (axes && axes->type == HU_JSON_OBJECT &&
        read_axis(axes, "lowercase_start_rate", &card.lowercase_start_rate) &&
        read_axis(axes, "no_terminal_punct_rate", &card.no_terminal_punct_rate) &&
        read_axis(axes, "question_rate", &card.question_rate) &&
        read_axis(axes, "exclamation_rate", &card.exclamation_rate) &&
        read_axis(axes, "emoji_rate", &card.emoji_rate)) {
        double n = hu_json_get_number(root, "n", 0.0);
        card.n = n > 0.0 ? (unsigned)n : 0u;
        hu_persona_card_copy_window(root, card.window_start, sizeof(card.window_start),
                                    card.window_end, sizeof(card.window_end));
        card.from_card = true;
        *out = card;
        err = HU_OK;
    }
    hu_json_free(alloc, root);
    return err;
}

hu_error_t hu_style_card_load_for_persona(hu_allocator_t *alloc, const char *name, size_t name_len,
                                          hu_style_card_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_style_card_default(out);
    char *buf = NULL;
    size_t got = 0;
    hu_error_t err = hu_persona_card_slurp(alloc, name, name_len, ".style-card.json", &buf, &got);
    if (err != HU_OK)
        return err;
    err = hu_style_card_parse(alloc, buf, got, out);
    alloc->free(alloc->ctx, buf, got + 1);
    return err;
}

void hu_style_card_resolve(const char *name, size_t name_len, hu_style_card_t *out) {
    if (!out)
        return;
    if (!name || name_len == 0) {
        hu_style_card_default(out);
        return;
    }
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_style_card_load_for_persona(&alloc, name, name_len, out);
    if (err == HU_OK)
        return;
    /* Once per process: the operator needs to know the prompt is running
     * on the compiled numbers, and how to fix it. */
    static atomic_bool warned_fallback = false;
    hu_log_warn_once(&warned_fallback, "persona", NULL,
                     "style card for persona '%.*s' %s (err=%d); rendering the casual "
                     "register from compiled defaults — run scripts/measure_style_card.py "
                     "--persona %.*s to write %.*s.style-card.json",
                     (int)name_len, name, err == HU_ERR_NOT_FOUND ? "missing" : "unreadable",
                     (int)err, (int)name_len, name, (int)name_len, name);
}

/* "about 1 in 8 texts" / "almost never" / "about 55% of texts". */
static void fmt_rate(double rate, char *out, size_t cap) {
    if (!(rate >= 0.005))
        snprintf(out, cap, "almost never");
    else if (rate >= 0.5)
        snprintf(out, cap, "about %d%% of texts", (int)lround(rate * 100.0));
    else
        snprintf(out, cap, "about 1 in %ld texts", lround(1.0 / rate));
}

hu_error_t hu_style_card_render_casual_rules(const hu_style_card_t *card, char *buf, size_t cap,
                                             size_t *out_len) {
    if (!card || !buf || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    char lower[40], question[40], emoji[40], exclaim[40];
    fmt_rate(card->lowercase_start_rate, lower, sizeof(lower));
    fmt_rate(card->question_rate, question, sizeof(question));
    fmt_rate(card->emoji_rate, emoji, sizeof(emoji));
    fmt_rate(card->exclamation_rate, exclaim, sizeof(exclaim));
    int n = snprintf(buf, cap,
                     "2. Normal capitalization (your phone capitalizes for you; a lowercase "
                     "start is %s); CAPS only when SHOUTING. About %d%% of your texts have "
                     "no period at the end — stop like a real text. Question marks only "
                     "when actually asking (%s). Emoji %s, exclamation points %s.\n",
                     lower, (int)lround(card->no_terminal_punct_rate * 100.0), question, emoji,
                     exclaim);
    if (n < 0 || (size_t)n + 1 > cap)
        return HU_ERR_OUT_OF_MEMORY;
    if (out_len)
        *out_len = (size_t)n;
    return HU_OK;
}
