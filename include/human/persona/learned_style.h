#ifndef HU_PERSONA_LEARNED_STYLE_H
#define HU_PERSONA_LEARNED_STYLE_H

/* Learned Style Profile — runtime half (Part B).
 *
 * The persona JSON hard-codes length rules ("Default 5-15 words", "MAX 15
 * words", "Be brief") that were written by hand and never re-measured. A
 * nightly learner (scripts/learned_style_*.py, Part A) measures how the owner
 * actually replies — per contact, per inbound shape — from his own sent
 * iMessages and writes ONLY numbers to
 *
 *     <persona dir>/<persona>.learned-style.json   (schema learned-style/v1)
 *
 * This module loads that file (mtime-cached, re-stat at most every 60 s),
 * answers lookups with a bucket -> contact -> global fallback, classifies the
 * inbound message's shape with the SAME rule the learner uses, renders one
 * second-person line for the prompt, and recognises the hand-written
 * length-imposing entries the line replaces.
 *
 * The persona JSON is never modified. The prompt behaviour is gated by
 * HU_LEARNED_STYLE=off|shadow|live (default off); see
 * docs/guides/learned-style-runtime.md and src/agent/turn/learned_style_turn.c.
 *
 * The contract shared with Part A lives in the PR description and the guide;
 * do not change the shape rule or schema on one side only. */

#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_LEARNED_STYLE_SCHEMA "learned-style/v1"
#define HU_LEARNED_STYLE_SUFFIX ".learned-style.json"
/* Re-stat the file at most this often; between stats the cache is trusted. */
#define HU_LEARNED_STYLE_RESTAT_SECS 60
/* A rate is "decisive" — worth a clause in the line — at or beyond these. */
#define HU_LEARNED_STYLE_RATE_LOW  0.2f
#define HU_LEARNED_STYLE_RATE_HIGH 0.8f

typedef enum { HU_LS_SHAPE_CASUAL = 0, HU_LS_SHAPE_QUESTION, HU_LS_SHAPE_STORY } hu_ls_shape_t;

/* Shape of the INBOUND message (trimmed): question if it contains '?';
 * else story if >= 140 bytes, or >= 80 bytes with >= 2 runs of '.'/'!';
 * else casual. NULL/empty -> casual. Identical to Part A's Python rule. */
hu_ls_shape_t hu_learned_style_shape(const char *inbound, size_t len);

/* "question" / "story" / "casual" — the bucket suffix and log token. */
const char *hu_learned_style_shape_name(hu_ls_shape_t shape);

typedef struct {
    bool found;        /* false: no file / wrong schema / no usable stats */
    bool from_bucket;  /* contact's "shape:<x>" bucket answered */
    bool from_contact; /* the contact (bucket or overall) answered; false = global */
    uint32_t n;
    float n_eff;
    uint16_t len_p25, len_p50, len_p90; /* reply length in bytes */
    float bubbles_p50;
    float lower_start_rate, emoji_rate, end_punct_rate;
    int32_t latency_p50_s; /* -1 when the file has null */
} hu_learned_style_t;

/* Which persona's file lookups read. The agent wiring calls this with
 * persona->name each turn; a change of name (or of HU_PERSONA_DIR) drops the
 * cache. NULL/0 clears it, after which every lookup is found=false. */
void hu_learned_style_set_persona(const char *name, size_t name_len);

/* Order: contacts[<id>].buckets["shape:<x>"] -> contacts[<id>].overall ->
 * global. Returns out->found. Never crashes on a missing or malformed file:
 * that is found=false, logged once at WARN. Thread-safe. */
bool hu_learned_style_lookup(const char *contact_id, size_t len, hu_ls_shape_t shape,
                             hu_learned_style_t *out);

/* Drop the cache (tests; persona reload). */
void hu_learned_style_cache_reset(void);

/* Test seam for the 60 s re-stat window: NULL restores time(). */
void hu_learned_style_set_clock(time_t (*now_fn)(void));

/* HU_LEARNED_STYLE, default OFF. */
hu_gate_mode_t hu_learned_style_mode(void);

/* Render the prompt line, e.g.
 *   "How you text Alex: usually about 25 characters, up to about 90;
 *    lowercase start most of the time; rarely end with punctuation."
 * A shape qualifier ("when they ask something" / "when they tell you
 * something big") is added only when the contact's own shape bucket
 * answered. Rate clauses appear only when decisive (<= 0.2 or >= 0.8).
 * first_name NULL/empty -> "them". Returns bytes written (NUL excluded), or
 * 0 when !ls->found or the buffer is too small. */
size_t hu_learned_style_render_line(const hu_learned_style_t *ls, hu_ls_shape_t shape,
                                    const char *first_name, size_t first_name_len, char *buf,
                                    size_t cap);

/* True when a hand-written persona entry imposes a FIXED length: a word or
 * character count ("5-15 words", "under 60 characters"), "MAX N", "one
 * line", or the words brief/short — except that brief/short inside a
 * mirroring rule ("match the energy… short message gets short reply") is
 * relative, not fixed, and is kept. */
bool hu_learned_style_is_length_rule(const char *s, size_t len);

/* Strip length-imposing sentences from a contact-profile context block
 * (hu_contact_profile_build_context output): only the "Dynamic:" line
 * (sentence by sentence; the line goes if nothing is left) and "Pattern:"
 * lines are examined, everything else is copied byte for byte. Writes to
 * out (cap >= len + 1) when out is non-NULL; returns the number of entries
 * removed either way. */
size_t hu_learned_style_strip_contact(const char *in, size_t len, char *out, size_t cap,
                                      size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_PERSONA_LEARNED_STYLE_H */
