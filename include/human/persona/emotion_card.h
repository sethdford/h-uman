#ifndef HU_PERSONA_EMOTION_CARD_H
#define HU_PERSONA_EMOTION_CARD_H

/* Emotion card — the MEASURED emotional register of the persona's own
 * texts, read from ~/.human/personas/<persona>.emotion-card.json (written
 * by scripts/measure_emotion_card.py; schema "emotion-card/v1").
 *
 * Framework: Cowen & Keltner's 27-category emotion taxonomy plus
 * "neutral", one label per message from the LOCAL judge on :8741, and a
 * distribution rather than a score. The style card measures how the
 * persona punctuates; this measures what the persona's texts feel like:
 * how often they are simply matter-of-fact, which feelings show when one
 * does, and how strongly.
 *
 * Unlike the style card there is NO compiled default. An invented emotion
 * baseline is exactly the "number nobody measured" that
 * no-number-without-a-measurement.md forbids, so a missing card means
 * "not measured" and the renderer is not called.
 *
 * Activation: HU_EMOTION_REGISTER (off | shadow | live, default off) —
 * see hu_emotion_register_mode. The rule is gated on
 * scripts/eval_emotion_register.py (nightly JSD of the twin's sent replies
 * against this card) plus a blind A/B round; do not flip it live on a
 * green build. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_EMOTION_CARD_TOP_MAX  4
#define HU_EMOTION_CARD_NAME_MAX 32

typedef struct hu_emotion_card_top {
    char emotion[HU_EMOTION_CARD_NAME_MAX]; /* taxonomy label, e.g. "amusement" */
    double share;                           /* fraction of texts, [0, 1] */
} hu_emotion_card_top_t;

typedef struct hu_emotion_card {
    double neutral_share;  /* texts labeled neutral, [0, 1] */
    double mean_intensity; /* mean judged intensity, [0, 1] */
    double valence_mean;   /* mean fixed per-category valence, [-1, 1] */
    hu_emotion_card_top_t top[HU_EMOTION_CARD_TOP_MAX]; /* most frequent non-neutral */
    unsigned top_count;
    unsigned n;            /* texts labeled (> 0 for a real card) */
    bool from_card;        /* true = loaded from a card file */
    char window_start[16]; /* YYYY-MM-DD */
    char window_end[16];
} hu_emotion_card_t;

/* Zero the card: from_card=false, n=0, no top entries. */
void hu_emotion_card_clear(hu_emotion_card_t *out);

/* Parse an emotion-card/v1 document. neutral_share.value and
 * mean_intensity.value must be in [0, 1], valence_mean.value in [-1, 1],
 * n > 0; otherwise HU_ERR_INVALID_ARGUMENT and *out is cleared. `top`
 * entries beyond HU_EMOTION_CARD_TOP_MAX or with malformed fields are
 * skipped, not fatal. */
hu_error_t hu_emotion_card_parse(hu_allocator_t *alloc, const char *json, size_t len,
                                 hu_emotion_card_t *out);

/* Load <hu_persona_base_dir()>/<name>.emotion-card.json. HU_ERR_NOT_FOUND
 * when absent; parse errors propagate. On any error *out is cleared. */
hu_error_t hu_emotion_card_load_for_persona(hu_allocator_t *alloc, const char *name,
                                            size_t name_len, hu_emotion_card_t *out);

/* true when a card was loaded. false (and *out cleared) when the persona
 * has no card — logged once per process so "not measured" is a visible
 * fact — or when the card is unreadable (warned once). NULL name -> false
 * silently. */
bool hu_emotion_card_resolve(const char *name, size_t name_len, hu_emotion_card_t *out);

/* Render casual rule 14 from the card's numbers. Deterministic text; the
 * only place those numbers reach the prompt. HU_ERR_INVALID_ARGUMENT for
 * a card with from_card=false. */
hu_error_t hu_emotion_card_render_rule(const hu_emotion_card_t *card, char *buf, size_t cap,
                                       size_t *out_len);

/* HU_EMOTION_REGISTER via hu_gate_mode_from_env; unset -> HU_GATE_OFF. */
hu_gate_mode_t hu_emotion_register_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* HU_PERSONA_EMOTION_CARD_H */
