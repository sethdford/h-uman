#ifndef HU_DAEMON_DIRECTOR_TAPBACK_H
#define HU_DAEMON_DIRECTOR_TAPBACK_H

/* Learned tapback evidence for director v2 (HU_DIRECTOR_V2).
 *
 * Whether Seth answers a message with only a reaction is not decided by a word
 * list. It comes from what he actually does, measured by the nightly
 * learned-style learner and written to
 *
 *     <persona dir>/<persona>.learned-style.json      (schema learned-style/v1)
 *
 * v1 of that schema has no reaction fields yet. This reader takes these
 * OPTIONAL fields from any stats object (a contact's "shape:<x>" bucket, the
 * contact's "overall", or "global"), and ignores them when absent:
 *
 *   tapback_only_rate        P(he replied with only a tapback | that cell), 0..1
 *   tapback_types            {"heart": r, "haha": r, ...} share of his tapbacks
 *   tapback_disengage_rate   after his tapback-only replies, the share where
 *                            they went quiet or pushed for a real answer
 *   tapback_disengage_n      how many tapback-only replies that rate is over
 *
 * Reconciliation: this is a minimal local reader so the director does not
 * depend on unmerged #586 (hu_learned_style_lookup). When #586 merges, the
 * shape rule and the file read move onto it, and the learner (#584 follow-up)
 * starts writing the fields above. */

#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/daemon/director.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The learner's inbound shape buckets: question if the contact's text has a
 * '?'; story if >= 140 bytes, or >= 80 bytes with >= 2 runs of '.'/'!'; else
 * casual. Lines the daemon injected ("[They sent a photo: ...]") are dropped
 * first. Same contract as the learner; see the reconciliation note above. */
typedef enum {
    HU_DIR_SHAPE_CASUAL = 0,
    HU_DIR_SHAPE_QUESTION,
    HU_DIR_SHAPE_STORY,
} hu_dir_shape_t;

hu_dir_shape_t hu_director_inbound_shape(const char *batch, size_t len);
const char *hu_director_shape_name(hu_dir_shape_t shape);

/* Where the final tapback-vs-text call came from, for the shadow log. */
typedef enum {
    HU_TAPBACK_SRC_NODATA = 0, /* no learned rate or no distribution: the model decided */
    HU_TAPBACK_SRC_MODEL,      /* learned data present, the model's choice stands */
    HU_TAPBACK_SRC_LEARNED,    /* learned data overrode a tapback-only choice to text */
} hu_tapback_src_t;

const char *hu_tapback_src_name(hu_tapback_src_t src);

/* Reaction names, in hu_tapback_profile_t.types order. */
#define HU_TAPBACK_KINDS 6
extern const char *const hu_tapback_kind_names[HU_TAPBACK_KINDS];

typedef struct {
    bool found;        /* tapback_only_rate read at some level */
    const char *level; /* "bucket", "contact" or "global" (static string) */
    float rate;
    uint32_t n; /* the level's "n" when present, else 0 */
    /* His own distribution: the lower quartile of tapback_only_rate over
     * every contact cell (each contact's shape buckets and overall). */
    bool cutoff_found;
    float cutoff;
    uint32_t cells;
    bool types_found;
    float types[HU_TAPBACK_KINDS];
    bool disengage_found;
    float disengage_rate;
    uint32_t disengage_n;
} hu_tapback_profile_t;

/* Fewest cells for the quartile to mean anything; below it there is no cutoff. */
#define HU_TAPBACK_MIN_CELLS 4u

/* Pure: read the evidence for (contact, shape) out of a parsed profile.
 * Lookup order per field: contact bucket "shape:<x>", contact "overall",
 * "global". Returns out->found. */
bool hu_tapback_profile_from_json(const hu_json_value_t *root, const char *contact,
                                  size_t contact_len, hu_dir_shape_t shape,
                                  hu_tapback_profile_t *out);

/* Read <persona dir>/<persona>.learned-style.json and call the above. A
 * missing, malformed or wrong-schema file is no data. */
bool hu_tapback_profile_load(hu_allocator_t *alloc, const char *persona, size_t persona_len,
                             const char *contact, size_t contact_len, hu_dir_shape_t shape,
                             hu_tapback_profile_t *out);

/* The evidence as one plain-fact line for the director prompt (no names),
 * e.g. "How Seth reacts with them, measured from his own texts: when they
 * ask something he replies with only a reaction 3% of the time (n=40)."
 * Returns bytes written, 0 when there is nothing to say. */
size_t hu_tapback_profile_facts(const hu_tapback_profile_t *p, hu_dir_shape_t shape, char *buf,
                                size_t cap);

/* The learned post-check. Overrides a tapback-only decision to text only when
 * his learned rate for this contact and shape is at or below the lower
 * quartile of his own distribution. No rate or no cutoff: never overrides
 * (HU_TAPBACK_SRC_NODATA). */
hu_tapback_src_t hu_director_v2_tapback_check(hu_director_result_t *result,
                                              const hu_tapback_profile_t *p);

#endif /* HU_DAEMON_DIRECTOR_TAPBACK_H */
