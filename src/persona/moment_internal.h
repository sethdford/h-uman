/* Internal-only header shared by src/persona/moment.c and
 * src/persona/moment_render.c. Contains the concrete definition of
 * hu_conversation_history_t (forward-declared in include/human/moment.h)
 * so both TUs can iterate entries without a public accessor explosion.
 *
 * Do NOT include this from outside src/persona/. If a Phase 3 production
 * call site needs to iterate history, expose targeted accessors in
 * include/human/moment.h instead. */

#ifndef HU_PERSONA_MOMENT_INTERNAL_H
#define HU_PERSONA_MOMENT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_HISTORY_TEXT_CAP 512

typedef struct hu_conversation_history_entry {
    int64_t ts_s;                   /* Unix timestamp, seconds */
    bool outbound;                  /* true = sent by us; false = inbound */
    char text[HU_HISTORY_TEXT_CAP]; /* message text, NUL-terminated */
} hu_conversation_history_entry_t;

struct hu_conversation_history_t {
    size_t count;
    hu_conversation_history_entry_t *entries; /* heap array[count] */
};

#endif /* HU_PERSONA_MOMENT_INTERNAL_H */
