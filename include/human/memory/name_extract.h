#ifndef HU_MEMORY_NAME_EXTRACT_H
#define HU_MEMORY_NAME_EXTRACT_H

#include "human/memory/graph.h"
#include <stdbool.h>
#include <stddef.h>

/* Zero-model per-turn name catcher (spec 2026-09-29 §4.2). Pure: no
 * allocation, no graph, no model. */

typedef enum hu_name_kind {
    HU_NAME_KNOWN = 0,   /* an existing entity name of this contact appears in the text */
    HU_NAME_CAPITALIZED, /* a new Capitalized 1-3 token run */
} hu_name_kind_t;

typedef struct hu_name_ref {
    const char *name;
    size_t len;
} hu_name_ref_t;

/* `name` points into the caller's `text` (CAPITALIZED) or into `known[]`
 * (KNOWN, the stored spelling) — valid only while those live. */
typedef struct hu_name_candidate {
    const char *name;
    size_t len;
    hu_name_kind_t kind;
} hu_name_candidate_t;

#define HU_NAME_MIN_LEN    2
#define HU_NAME_MAX_LEN    40
#define HU_NAME_MAX_TOKENS 3

/* Entities that count as NAMES (spec §4.2 KNOWN, §4.6 LIVE seeding):
 * PERSON, PLACE, ORGANIZATION, EVENT in any case, or an UNKNOWN whose first
 * byte is an ASCII capital. Never TOPIC or EMOTION. */
bool hu_name_entity_is_nameable(hu_entity_type_t type, const char *name, size_t len);

/* Up to `out_cap` candidates from `text`, KNOWN first, one per name
 * (case-insensitive):
 *  - KNOWN: a `known` name found case-insensitively at word boundaries.
 *  - CAPITALIZED: a run of 1-3 tokens separated by single spaces, each an
 *    ASCII capital followed by a lowercase letter ("Priya's" -> "Priya"),
 *    not at a sentence start (text start or after . ! ? newline), not a
 *    stopword (I, days, months, greetings Hey/Hi/Ok/Lol/Yeah/Thanks, God;
 *    "Mom"/"Dad" allowed), 2-40 bytes, and not glued to a non-ASCII byte. */
size_t hu_name_extract(const char *text, size_t len, const hu_name_ref_t *known, size_t known_count,
                       hu_name_candidate_t *out, size_t out_cap);

#endif /* HU_MEMORY_NAME_EXTRACT_H */
