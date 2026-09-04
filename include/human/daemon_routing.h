#ifndef HU_DAEMON_ROUTING_H
#define HU_DAEMON_ROUTING_H

#include "channel_loop.h"
#include "core/allocator.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Daemon message routing utilities extracted from daemon.c.
 *
 * Public API (hu_daemon_set_missed_msg_threshold, hu_missed_message_acknowledgment,
 * hu_daemon_photo_viewing_delay_ms, hu_daemon_video_viewing_delay_ms) is in daemon.h.
 */

/* Tapback-worthy: message properties suggest acknowledgment, not response.
 * Uses structural checks (length, question mark, word count). */
bool hu_daemon_is_tapback_worthy(const char *msg, size_t len);

/* Photo viewing delay: returns 3-8 s (ms) if batch has attachment, else 0. */
uint32_t hu_daemon_compute_photo_delay(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                       size_t batch_end, uint32_t seed);

/* Video viewing delay: returns 2-10 s (ms) if batch has video, else 0. */
uint32_t hu_daemon_compute_video_delay(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                       size_t batch_end, uint32_t seed);

struct hu_config; /* human/config.h */

/* Compiled default for daemon-side reflexive LLM calls (double-text, GIF/music/
 * image suggestions) when neither the agent's model_name nor the model router's
 * reflexive tier is configured. Update here on the next Gemini deprecation. */
#define HU_DAEMON_FALLBACK_MODEL_DEFAULT "gemini-3.1-flash-lite"

/* Fallback model resolver: returns config->agent.mr_reflexive_model when set and
 * non-empty, else HU_DAEMON_FALLBACK_MODEL_DEFAULT. Never returns NULL. len_out
 * (optional) receives strlen of the returned string. */
const char *hu_daemon_fallback_model(const struct hu_config *config, size_t *len_out);

#endif /* HU_DAEMON_ROUTING_H */
/* Task 8: rate + per-contact cooldown around hu_missed_message_acknowledgment.
 * Returns the phrase only when the legacy predicate fires AND the seed draw is
 * below the configured rate AND no filler went to `contact_key` within the
 * cooldown. Records the emission when it returns non-NULL. */
const char *hu_missed_message_acknowledgment_gated(int64_t delay_secs, int receive_hour,
                                                   int current_hour, uint32_t seed,
                                                   const char *contact_key, size_t contact_key_len,
                                                   int64_t now);
void hu_daemon_set_missed_msg_ack_rate(double rate);
void hu_daemon_set_missed_msg_ack_cooldown(uint32_t secs);
void hu_daemon_filler_reset_for_test(void);
