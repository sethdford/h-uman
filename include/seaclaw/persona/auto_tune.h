#ifndef SC_PERSONA_AUTO_TUNE_H
#define SC_PERSONA_AUTO_TUNE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include <stddef.h>

/* Aggregate recent replay insights from memory into a cumulative feedback summary.
 * Reads all stored replay_insights entries, deduplicates observations,
 * and builds a ranked summary of what's working and what's not.
 * Caller owns returned string. */
sc_error_t sc_replay_auto_tune(sc_allocator_t *alloc, sc_memory_t *memory,
                              const char *contact_id, size_t contact_id_len,
                              char **summary_out, size_t *summary_len);

/* Build a tone calibration prompt from accumulated feedback.
 * Returns a context injection string for the system prompt. Caller owns. */
char *sc_replay_tune_build_context(sc_allocator_t *alloc, const char *summary,
                                  size_t summary_len, size_t *out_len);

#endif
