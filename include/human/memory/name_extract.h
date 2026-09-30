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

/* A known entity name. `exact_case`: match only the stored spelling, not any
 * casing. Set for rows the per-turn catcher itself created and nothing has
 * confirmed yet (UNKNOWN + names:turn), so a Capitalized word it once caught
 * ("Going") cannot then match every lowercase "going" and feed on itself. */
typedef struct hu_name_ref {
    const char *name;
    size_t len;
    bool exact_case;
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
 *  - KNOWN: a `known` name found at word boundaries, case-insensitively
 *    unless `exact_case`. A byte >= 0x80 is a word byte ("Ren" is not in
 *    "René"), except the non-letter sequences below.
 *  - CAPITALIZED: a run of 1-3 tokens separated by single spaces, each an
 *    ASCII capital followed by a lowercase letter. A token never ends in an
 *    apostrophe: a trailing ' or U+2019 and a possessive 's / ’s are
 *    stripped ("Priya’s" -> "Priya", "Chris'" -> "Chris"). A hyphen between
 *    two Capitalized parts joins them ("Mary-Kate"). Not at a sentence start
 *    (text start, or after . ! ? newline, an emoji, U+FFFC or U+2026), not a
 *    stopword (pronoun contractions, days, months, greetings and filler;
 *    "Mom"/"Dad" allowed), 2-40 bytes, and not glued to a non-ASCII letter
 *    ("José" gives no "Jos"). Emoji, U+FFFC and U+2000-U+203F punctuation
 *    are boundaries, not letters. */
size_t hu_name_extract(const char *text, size_t len, const hu_name_ref_t *known, size_t known_count,
                       hu_name_candidate_t *out, size_t out_cap);

#endif /* HU_MEMORY_NAME_EXTRACT_H */
