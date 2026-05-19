/* include/human/channels/contact_signature.h
 *
 * Per-contact relationship signatures: a structured aggregate over chat.db
 * history that captures HOW you communicate with each contact — median
 * response latency, conversation initiation balance, time-of-day
 * distribution, message length, exchange frequency.
 *
 * Today's persona learning knows "you talk to Alice" + "Alice loves
 * hiking." It doesn't know that you usually message Alice on weekday
 * evenings, that she replies within 10 minutes during the day but hours
 * at night, or that you initiate 70% of conversations. A human friend
 * gradually learns these; this module computes them deterministically.
 *
 * Two-layer API (mirrors imessage_gaps): pure helpers (TOD bucketing,
 * median latency, initiation ratio) compile + test on every build; the
 * SQL-backed compute paths are gated under HU_IS_TEST / non-Apple /
 * no-SQLite and return HU_ERR_NOT_SUPPORTED on stubbed builds. */

#ifndef HU_CHANNELS_CONTACT_SIGNATURE_H
#define HU_CHANNELS_CONTACT_SIGNATURE_H

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_CONTACT_SIGNATURE_HANDLE_MAX 128

/* Time-of-day buckets. Local-hour-based. NIGHT spans the late-evening /
 * early-morning slot when you'd typically not message — useful for
 * detecting "you never message Alice after 22:00" patterns. */
typedef enum {
    HU_TOD_NIGHT = 0, /* hours 0-5  (00:00..05:59 local) */
    HU_TOD_MORNING,   /* hours 6-11 (06:00..11:59 local) */
    HU_TOD_AFTERNOON, /* hours 12-17 (12:00..17:59 local) */
    HU_TOD_EVENING,   /* hours 18-23 (18:00..23:59 local) */
    HU_TOD_BUCKET_COUNT
} hu_tod_bucket_t;

typedef struct {
    char contact_handle[HU_CONTACT_SIGNATURE_HANDLE_MAX];
    uint32_t total_messages; /* lifetime */
    uint32_t messages_last_30_days;
    uint32_t outbound_count;             /* is_from_me=1 */
    uint32_t inbound_count;              /* is_from_me=0 */
    double initiation_ratio;             /* (you-started / total convos); 0.5 = equal */
    int32_t median_response_latency_sec; /* their reply time to your messages; -1 if unknown */
    int32_t avg_message_length;          /* characters, averaged across all messages */
    uint32_t tod_distribution[HU_TOD_BUCKET_COUNT]; /* messages-by-time-of-day */
    int32_t weekday_skew_pct;                       /* % messages M-F vs weekend; 50 = even */
    int64_t first_seen_unix;
    int64_t last_seen_unix;
    int32_t active_days; /* distinct days with at least one message */
} hu_contact_signature_t;

/* --- PURE helpers (testable without SQLite) --- */

/* Bucket a Unix timestamp into a time-of-day bucket using the device's
 * local timezone offset (e.g. -28800 for PST). Pass 0 for UTC. */
hu_tod_bucket_t hu_tod_bucket_from_unix(int64_t ts_unix, int32_t tz_offset_seconds);

/* Compute median latency from a SORTED array of inter-message delays
 * (seconds). Returns -1 for empty input.
 *
 * Even count → average of the two middle values (rounded toward zero).
 * Odd count  → middle value. */
int32_t hu_signature_median_latency(const int32_t *latencies_sorted, size_t count);

/* Compute the initiation ratio from a SORTED-BY-TIME sequence of
 * messages. A "conversation" is a sequence of messages with less than
 * gap_threshold_sec between consecutive messages; the first message of
 * each conversation is the initiator. The ratio is
 * (your-initiations / total-conversations).
 *
 * Returns 0.5 (neutral) when count == 0 or when no conversations
 * are detected (single-message input). */
double hu_signature_initiation_ratio(const int64_t *message_timestamps_sorted,
                                     const bool *is_from_me, size_t count,
                                     int32_t gap_threshold_sec);

/* --- SQL-backed compute paths --- */

/* Compute a signature from chat.db for ONE contact. Returns HU_OK on
 * success (empty struct if contact has no messages); HU_ERR_NOT_SUPPORTED
 * under HU_IS_TEST or non-Apple/no-SQLite builds. */
hu_error_t hu_contact_signature_compute(const char *db_path, const char *contact_handle,
                                        int64_t now_unix, hu_contact_signature_t *out);

/* Compute signatures for the top-N most-active contacts (by total
 * lifetime message count). out_n receives the actual number written
 * (≤ cap). Same gating as `_compute`. */
hu_error_t hu_contact_signature_top_n(const char *db_path, int64_t now_unix, size_t n,
                                      hu_contact_signature_t *out, size_t cap, size_t *out_n);

#ifdef __cplusplus
}
#endif

#endif /* HU_CHANNELS_CONTACT_SIGNATURE_H */
