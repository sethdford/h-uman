/**
 * daemon_routing.c — Message routing utilities for the daemon service loop.
 *
 * Extracted from daemon.c. Implements:
 *   - Tapback detection (structural heuristic)
 *   - Photo/video viewing delays (F6/F7)
 *   - Missed message acknowledgment (F10)
 */
#include "human/daemon_routing.h"
#include "human/daemon.h"

#include <stdint.h>
#include <string.h>

/* ── Tapback detection ───────────────────────────────────────────────── */

bool hu_daemon_is_tapback_worthy(const char *msg, size_t len) {
    if (!msg || len == 0)
        return false;
    /* Question invites response — never tapback */
    if (memchr(msg, '?', len))
        return false;
    /* Very short (<=6 chars), no question: likely acknowledgment */
    if (len <= 6)
        return true;
    /* Single token (no space): emoji, "k", "lol", etc. */
    if (len <= 12) {
        for (size_t i = 0; i < len; i++) {
            if (msg[i] == ' ')
                return false;
        }
        return true;
    }
    /* Substantive message (>20 chars) invites response */
    if (len > 20)
        return false;
    /* Short multi-word but no question: borderline — treat as tapback if very brief */
    return len <= 12;
}

/* ── Photo viewing delay (F6) ────────────────────────────────────────── */
#define HU_PHOTO_VIEWING_DELAY_MIN_MS   3000
#define HU_PHOTO_VIEWING_DELAY_RANGE_MS 5001 /* 0..5000 → 3–8 s inclusive */

uint32_t hu_daemon_compute_photo_delay(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                       size_t batch_end, uint32_t seed) {
    for (size_t b = batch_start; b <= batch_end; b++) {
        if (msgs[b].has_attachment)
            return HU_PHOTO_VIEWING_DELAY_MIN_MS + (seed % HU_PHOTO_VIEWING_DELAY_RANGE_MS);
    }
    return 0;
}

/* ── Video viewing delay (F7) ────────────────────────────────────────── */
#define HU_VIDEO_VIEWING_DELAY_MIN_MS   2000
#define HU_VIDEO_VIEWING_DELAY_RANGE_MS 8001 /* 0..8000 → 2–10 s inclusive */

uint32_t hu_daemon_compute_video_delay(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                       size_t batch_end, uint32_t seed) {
    for (size_t b = batch_start; b <= batch_end; b++) {
        if (msgs[b].has_video)
            return HU_VIDEO_VIEWING_DELAY_MIN_MS + (seed % HU_VIDEO_VIEWING_DELAY_RANGE_MS);
    }
    return 0;
}

/* ── Test hooks (forward to compute functions) ───────────────────────── */
#ifdef HU_IS_TEST
uint32_t hu_daemon_photo_viewing_delay_ms(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                          size_t batch_end, uint32_t seed) {
    return hu_daemon_compute_photo_delay(msgs, batch_start, batch_end, seed);
}
uint32_t hu_daemon_video_viewing_delay_ms(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                          size_t batch_end, uint32_t seed) {
    return hu_daemon_compute_video_delay(msgs, batch_start, batch_end, seed);
}
#endif

/* ── Missed-message acknowledgment (F10) ─────────────────────────────── */

static uint32_t g_missed_msg_threshold_sec = 30 * 60;

void hu_daemon_set_missed_msg_threshold(uint32_t secs) {
    if (secs >= 60)
        g_missed_msg_threshold_sec = secs;
}

const char *hu_missed_message_acknowledgment(int64_t delay_secs, int receive_hour, int current_hour,
                                             uint32_t seed) {
    if (delay_secs <= (int64_t)g_missed_msg_threshold_sec)
        return NULL;

    /* Natural gap: received 2AM–6AM, responding 8AM+ → "just woke up" style */
    if (receive_hour >= 2 && receive_hour < 6 && current_hour >= 8) {
        static const char *const woke[] = {"ha just woke up", "omg just woke up",
                                           "oof just woke up"};
        return woke[seed % 3];
    }

    /* Default: missed/saw-this phrases */
    static const char *const missed[] = {"sorry just saw this", "oh man missed this",
                                         "ha my bad just saw this"};
    return missed[seed % 3];
}
