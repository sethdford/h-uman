#ifndef HU_DAEMON_DIRECTOR_TAPBACK_H
#define HU_DAEMON_DIRECTOR_TAPBACK_H

/* What Seth actually does, for director v2 (HU_DIRECTOR_V2): his learned
 * tapback behaviour and reply delay for this contact and message shape.
 *
 * Nothing here is a word list or a fixed rule. The numbers come from the
 * nightly learned-style learner (#584), written to
 *
 *     <persona dir>/<persona>.learned-style.json      (schema learned-style/v1)
 *
 * Read from any stats object (a contact's "shape:<x>" bucket, the contact's
 * "overall", or "global"), in that order:
 *
 *   n                        sample count (the learner writes it on every level)
 *   latency_p50_s            his median reply delay (v1 field; may be null)
 *   tapback_only_rate        P(he replied with only a tapback | that cell)   OPTIONAL
 *   tapback_types            {"heart": r, "haha": r, ...}                    OPTIONAL
 *   tapback_disengage_rate   after his tapback-only replies, the share where
 *                            they went quiet or pushed for a real answer     OPTIONAL
 *   tapback_disengage_n      how many replies that rate is over              OPTIONAL
 *   voice_memo_rate / gif_rate / share_rate
 *                            how often his replies are a voice memo, a GIF,
 *                            or carry a shared song/video                    OPTIONAL
 *
 * The OPTIONAL fields are not in v1 yet; absent means no data.
 *
 * RECONCILIATION with #586 (learned-style runtime): hu_director_inbound_shape
 * now IS hu_learned_style_shape_inbound (the 2026-10-02 train deleted the
 * mirror). The mtime-cached reader still mirrors #586's cache; the parity
 * cases in tests/test_director_tapback.c are #586's own and must keep
 * passing. */

#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/daemon/director.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    HU_DIR_SHAPE_CASUAL = 0,
    HU_DIR_SHAPE_QUESTION,
    HU_DIR_SHAPE_STORY,
} hu_dir_shape_t;

/* Shape of what the contact typed in the batch: daemon-injected note lines
 * ("[They sent a photo: ...]", "[Audio transcription: ...]", ...) are dropped,
 * the rest re-joined with '\n' and trimmed; question if it has '?', story if
 * >= 140 bytes or >= 80 bytes with >= 2 runs of '.'/'!', else casual. */
hu_dir_shape_t hu_director_inbound_shape(const char *batch, size_t len);
const char *hu_director_shape_name(hu_dir_shape_t shape);

/* Where the final tapback-vs-text call came from, for the shadow log. */
typedef enum {
    HU_TAPBACK_SRC_NODATA = 0, /* no learned rate, or no comparable cutoff: the model decided */
    HU_TAPBACK_SRC_MODEL,      /* learned data present, the model's choice stands */
    HU_TAPBACK_SRC_LEARNED,    /* learned data turned a tapback-only choice into text */
} hu_tapback_src_t;

const char *hu_tapback_src_name(hu_tapback_src_t src);

#define HU_TAPBACK_KINDS 6
#define HU_FORM_KINDS    3 /* voice_memo_rate, gif_rate, share_rate */
extern const char *const hu_tapback_kind_names[HU_TAPBACK_KINDS];

/* Minimum samples for a cell, the learner's own thresholds
 * (scripts/learned_style_profile.py in #584: MIN_BUCKET_N = 3 for a
 * contact's shape bucket, MIN_CONTACT_N = 5 for a contact). A cell below them
 * is noise to the learner, so it is noise here too. */
#define HU_TAPBACK_MIN_BUCKET_N  3u
#define HU_TAPBACK_MIN_CONTACT_N 5u
/* A lower quartile needs at least four cells to have a lower quarter. */
#define HU_TAPBACK_MIN_CELLS 4u

typedef struct {
    bool found;        /* tapback_only_rate read at some level with enough n */
    const char *level; /* "bucket", "contact", "global" or "none" (static) */
    float rate;
    uint32_t n;
    /* The cutoff comes from cells at the SAME level as `level`: every
     * contact's shape buckets for a bucket lookup, every contact's overall
     * for a contact lookup. A global lookup has no peer cells: no cutoff. */
    bool cutoff_found;
    float cutoff;
    uint32_t cells;
    bool types_found;
    float types[HU_TAPBACK_KINDS];
    bool disengage_found;
    float disengage_rate;
    uint32_t disengage_n;
    bool latency_found;
    int32_t latency_s;
    /* voice memo, GIF, share; bit f of form_found set when form_rates[f] was read */
    unsigned form_found;
    float form_rates[HU_FORM_KINDS];
} hu_tapback_profile_t;

/* Pure: read the evidence for (contact, shape) out of a parsed profile.
 * Returns true when anything usable was found (rate, latency, types or
 * disengagement). */
bool hu_tapback_profile_from_json(const hu_json_value_t *root, const char *contact,
                                  size_t contact_len, hu_dir_shape_t shape,
                                  hu_tapback_profile_t *out);

/* Read <persona dir>/<persona>.learned-style.json (parsed once per file
 * version: mtime + size cache, thread-safe) and call the above. A missing,
 * malformed or wrong-schema file is no data. */
bool hu_tapback_profile_load(const char *persona, size_t persona_len, const char *contact,
                             size_t contact_len, hu_dir_shape_t shape, hu_tapback_profile_t *out);

/* Drop the cache (tests). */
void hu_tapback_profile_cache_reset(void);

/* The evidence as one plain-fact line for the director prompt (no names),
 * e.g. "How Seth replies, measured from his own texts: he usually answers
 * them after about 4 minutes; with them, when they ask something he replies
 * with only a reaction 3% of the time (n=40)." Returns bytes, 0 if nothing. */
size_t hu_tapback_profile_facts(const hu_tapback_profile_t *p, hu_dir_shape_t shape, char *buf,
                                size_t cap);

/* The learned post-check. A tapback-only decision becomes text only when his
 * rate for this contact and shape is at or below the n-weighted lower
 * quartile of his own same-level cells. The model's own delay and direction
 * are kept. No rate or no comparable cutoff: never overrides. If every peer
 * cell is 0 (he never answers with only a tapback), the cutoff is 0 and every
 * tapback-only choice is overridden: that is what his data says. */
hu_tapback_src_t hu_director_v2_tapback_check(hu_director_result_t *result,
                                              const hu_tapback_profile_t *p);

#endif /* HU_DAEMON_DIRECTOR_TAPBACK_H */
