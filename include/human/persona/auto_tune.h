#ifndef HU_PERSONA_AUTO_TUNE_H
#define HU_PERSONA_AUTO_TUNE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>

/* Aggregate recent replay insights from memory into a cumulative feedback summary.
 * Reads all stored replay_insights entries, deduplicates observations,
 * and builds a ranked summary of what's working and what's not.
 * Caller owns returned string. */
hu_error_t hu_replay_auto_tune(hu_allocator_t *alloc, hu_memory_t *memory,
                              const char *contact_id, size_t contact_id_len,
                              char **summary_out, size_t *summary_len);

/* Build a tone calibration prompt from accumulated feedback.
 * Returns a context injection string for the system prompt. Caller owns. */
char *hu_replay_tune_build_context(hu_allocator_t *alloc, const char *summary,
                                  size_t summary_len, size_t *out_len);

#endif
