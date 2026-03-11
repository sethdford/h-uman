#ifndef HU_CONTEXT_HUMOR_H
#define HU_CONTEXT_HUMOR_H

#include "human/core/allocator.h"
#include "human/persona.h"
#include <stdbool.h>
#include <stddef.h>

/* Build full humor directive from persona humor profile.
 * Returns NULL if emotion matches never_during or conversation not playful. */
char *hu_humor_build_persona_directive(hu_allocator_t *alloc,
                                      const hu_humor_profile_t *humor,
                                      const char *dominant_emotion, size_t emotion_len,
                                      bool conversation_playful, size_t *out_len);

#endif /* HU_CONTEXT_HUMOR_H */
