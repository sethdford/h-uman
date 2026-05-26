#ifndef HUMAN_PERSONA_PACING_H
#define HUMAN_PERSONA_PACING_H

#include "human/persona.h" /* hu_persona_t */
#include <stdint.h>

/* Record the start of a reply-pacing window. Caller passes a stack-allocated
 * uint64_t to record start time. Pair with hu_persona_pace_reply_finish. */
void hu_persona_pace_reply_start(uint64_t *start_ms_out);

/* Sleep just enough so the elapsed wall-clock since pace_reply_start is at
 * least persona->min_reply_delay_ms * 1.2 (jittered by
 * persona->reply_delay_variance_ms). If already past that threshold, returns
 * immediately (no negative sleep). */
void hu_persona_pace_reply_finish(const hu_persona_t *persona, uint64_t start_ms);

#endif
