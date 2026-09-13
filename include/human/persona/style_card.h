#ifndef HU_PERSONA_STYLE_CARD_H
#define HU_PERSONA_STYLE_CARD_H

/* Style card — the MEASURED per-axis style statistics the prompt renders
 * from, read from ~/.human/personas/<persona>.style-card.json (written by
 * scripts/measure_style_card.py over a window of the user's own outbound
 * texts; schema "style-card/v2").
 *
 * Why a single source: on 2026-09-03 the same axis carried three different
 * numbers (a C comment, the old card, seth.json) and none matched the
 * measurement. The July 2026 deliberation leak was the model agonizing over
 * exactly such a contradiction between prompt layers. The card is now the
 * only place a style number lives; the compiled default below is a fallback
 * for a missing card and announces itself in the log when used.
 *
 * All rates are per-message fractions in [0, 1]. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_style_card {
    double lowercase_start_rate;   /* first letter is lowercase */
    double no_terminal_punct_rate; /* message ends with no . ? ! … */
    double question_rate;          /* ends with '?' */
    double exclamation_rate;       /* ends with '!' */
    double emoji_rate;             /* contains >= 1 emoji */
    unsigned n;                    /* messages measured (0 for the default) */
    /* Judge-free pair axis (scripts/reply_pairs.py): how the persona answers
     * a LONG or question-bearing inbound. substantive_n == 0 when the card
     * predates the axis; nothing renders below HU_STYLE_CARD_SUBSTANTIVE_MIN_N. */
    unsigned substantive_n;
    unsigned substantive_median_chars;
    double substantive_share_short;       /* share of those replies <= 60 chars */
    double substantive_answer_first_rate; /* share opening with yes/no/sure/idk… */
    bool from_card;                       /* true = loaded from a card file */
    char window_start[16];                /* YYYY-MM-DD, empty for the default */
    char window_end[16];
} hu_style_card_t;

/* Compiled fallback. from_card=false, n=0. Values are the last measurement
 * taken when this file was edited (see style_card.c) — a stale-but-honest
 * baseline, NOT authoritative. */
void hu_style_card_default(hu_style_card_t *out);

/* Parse a style-card/v2 JSON document. Every axis the renderer needs must
 * be present with a value in [0, 1]; otherwise HU_ERR_INVALID_ARGUMENT and
 * *out is left as the compiled default. */
hu_error_t hu_style_card_parse(hu_allocator_t *alloc, const char *json, size_t len,
                               hu_style_card_t *out);

/* Load <hu_persona_base_dir()>/<name>.style-card.json. HU_ERR_NOT_FOUND
 * when the file is absent; parse errors propagate. On any error *out is
 * the compiled default. */
hu_error_t hu_style_card_load_for_persona(hu_allocator_t *alloc, const char *name, size_t name_len,
                                          hu_style_card_t *out);

/* Card if present, else default — logs once per process when it falls
 * back so a missing card is a visible fact, not a silent regression to
 * stale numbers. NULL name -> default (no log). */
void hu_style_card_resolve(const char *name, size_t name_len, hu_style_card_t *out);

/* Render the casual register's rule 2 (capitalization / terminal
 * punctuation / question / emoji / exclamation) from the card's numbers.
 * Deterministic text; the only place those numbers reach the prompt. */
hu_error_t hu_style_card_render_casual_rules(const hu_style_card_t *card, char *buf, size_t cap,
                                             size_t *out_len);

#define HU_STYLE_CARD_SUBSTANTIVE_MIN_N 20

/* Rule 15 — the MEASURED substantive register (2026-09-13). The multi-turn
 * nightly's debate / news / advice scenarios end judged "AI" for two named
 * reasons: every reply is "[reaction] — [rephrase of what they said]" and the
 * twin agrees with everything. The persona's real replies to a long or
 * question-bearing text answer first, take a position, and stay short. Only
 * renders from a card with substantive_n >= HU_STYLE_CARD_SUBSTANTIVE_MIN_N;
 * HU_ERR_INVALID_ARGUMENT otherwise. Gated by hu_substantive_register_mode. */
hu_error_t hu_style_card_render_substantive_rule(const hu_style_card_t *card, char *buf, size_t cap,
                                                 size_t *out_len);

/* HU_SUBSTANTIVE_REGISTER via hu_gate_mode_from_env; unset -> HU_GATE_OFF.
 * Measurement that gates it: scripts/eval_multiturn_local.py --persona-prompt
 * production --repeats 3 --scenarios debate_opinions,news_reaction_chain,
 * advice_seeking, off vs live, scored by last-third mean. */
hu_gate_mode_t hu_substantive_register_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* HU_PERSONA_STYLE_CARD_H */
