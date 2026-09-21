#ifndef HU_VOICE_SESSION_H
#define HU_VOICE_SESSION_H

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/voice/duplex.h"
#include "human/voice/provider.h"
#include "human/voice/realtime.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_VOICE_TARGET_FIRST_BYTE_MS 200
#define HU_VOICE_TARGET_ROUND_TRIP_MS 500
#define HU_VOICE_TARGET_INTERRUPT_MS  100

/** Unified duplex FSM + optional OpenAI Realtime WebSocket session. */
typedef struct hu_voice_session {
    hu_duplex_session_t duplex;
    hu_voice_rt_session_t *rt;    /* legacy direct pointer (NULL when using provider) */
    hu_voice_provider_t provider; /* vtable-based voice backend (preferred) */
    hu_turn_action_t last_action;
    bool active;
    int64_t started_at;
    int64_t last_audio_ms;
    char channel_name[32];
    struct {
        int64_t first_byte_ms;        /* time from send_audio to first response byte */
        int64_t total_round_trip_ms;  /* full round trip */
        int64_t interrupt_latency_ms; /* time from interrupt to silence */
        size_t measurements;
        double avg_first_byte_ms;
    } latency;
    /* Running averages for round-trip / interrupt (separate sample counts). */
    double latency_avg_round_trip_ms;
    double latency_avg_interrupt_ms;
    size_t latency_round_trip_measurements;
    size_t latency_interrupt_measurements;
    /* Internal marks for latency (not part of public contract). */
    int64_t latency_send_mark_ms;
    int64_t latency_rt_mark_ms;
    bool latency_first_byte_pending;
    int64_t latency_interrupt_mark_ms;
    bool latency_await_interrupt_silence;
} hu_voice_session_t;

hu_error_t hu_voice_session_start(hu_allocator_t *alloc, hu_voice_session_t *session,
                                  const char *channel_name, size_t channel_name_len,
                                  const hu_config_t *config);
hu_error_t hu_voice_session_stop(hu_voice_session_t *session);
hu_error_t hu_voice_session_send_audio(hu_voice_session_t *session, const uint8_t *pcm16,
                                       size_t pcm16_len);
hu_error_t hu_voice_session_on_interrupt(hu_voice_session_t *session);

hu_error_t hu_voice_session_get_latency(const hu_voice_session_t *session,
                                        int64_t *out_avg_first_byte_ms,
                                        int64_t *out_avg_round_trip_ms,
                                        int64_t *out_avg_interrupt_ms);

/** Call when the first byte/chunk of assistant audio arrives (e.g. Realtime delta). */
void hu_voice_session_note_response_first_byte(hu_voice_session_t *session);

/** Call when the assistant audio response is complete. */
void hu_voice_session_note_response_complete(hu_voice_session_t *session);

/** Call when output has gone silent after an interrupt (barge-in settled). */
void hu_voice_session_note_interrupt_silence(hu_voice_session_t *session);

/**
 * If average first-byte latency exceeds HU_VOICE_TARGET_FIRST_BYTE_MS, log once to stderr.
 * Safe no-op when there are no measurements or session is inactive.
 */
void hu_voice_session_warn_first_byte_latency_if_needed(const hu_voice_session_t *session);

/* ── Micro-turn API ─────────────────────────────────────────────── */

/**
 * Inject a user-side turn signal into the duplex FSM.
 * Returns the recommended action in *out_action.
 */
hu_error_t hu_voice_session_user_turn_signal(hu_voice_session_t *session, hu_turn_signal_t signal,
                                             hu_turn_action_t *out_action);

/**
 * Inject an agent-side turn signal into the duplex FSM.
 * Returns the recommended action in *out_action.
 */
hu_error_t hu_voice_session_agent_turn_signal(hu_voice_session_t *session, hu_turn_signal_t signal,
                                              hu_turn_action_t *out_action);

/** Retrieve the action recommended by the most recent FSM transition. */
hu_turn_action_t hu_voice_session_last_action(const hu_voice_session_t *session);

hu_error_t hu_voice_session_recv_event(hu_voice_session_t *session, hu_allocator_t *alloc,
                                       hu_voice_rt_event_t *out, int timeout_ms);
hu_error_t hu_voice_session_activity_start(hu_voice_session_t *session);
hu_error_t hu_voice_session_activity_end(hu_voice_session_t *session);
hu_error_t hu_voice_session_send_tool_response(hu_voice_session_t *session, const char *name,
                                               const char *call_id, const char *response_json);

#endif
