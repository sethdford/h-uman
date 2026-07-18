#ifndef HU_BEHAVIOR_TAPBACK_BAND_H
#define HU_BEHAVIOR_TAPBACK_BAND_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Tapback timing bands (roadmap #18) — humans tapback within
 * seconds-to-minutes or not at all. A tapback dispatched after the measured
 * latency band reads as a tell (and renders as a `Loved "..."` text echo on
 * SMS). The band is measured from Seth's OWN chat.db history by
 * scripts/tapback_bands.py and persisted at ~/.human/tapback_bands.json.
 *
 * The drop decision is a pure predicate so the truth table is testable
 * without the daemon loop (see security-predicate-extraction rule).
 */

typedef struct hu_tapback_band {
    bool valid;     /* true when a band was found in the bands file */
    double rate;    /* tapbacks per received message (informational) */
    int64_t p50_ms; /* median observed tapback latency */
    int64_t p90_ms; /* p90 observed latency — the drop cap when valid */
} hu_tapback_band_t;

/* Cap when no bands file / no usable band: 15 minutes. */
#define HU_TAPBACK_DEFAULT_CAP_MS (15LL * 60LL * 1000LL)

/* Pure predicate: may a tapback for a message originated at target_msg_ms
 * still be sent at now_ms? Unknown origin (target_msg_ms <= 0) and negative
 * age (clock skew) are within band; otherwise age <= cap, where cap is the
 * band's p90_ms when valid, else HU_TAPBACK_DEFAULT_CAP_MS. */
bool hu_tapback_within_band(int64_t now_ms, int64_t target_msg_ms, const hu_tapback_band_t *band);

/* Dispatch-level wrapper for the daemon send path (second granularity).
 * msg_timestamp_sec is hu_channel_loop_msg_t.timestamp_sec; 0 means
 * "origin unknown / fresh at poll" and is within band. */
bool hu_tapback_dispatch_within_band(int64_t now_sec, int64_t msg_timestamp_sec,
                                     const hu_tapback_band_t *band);

/* Parse a bands JSON document and select the band for contact_id
 * ("contacts" map, falling back to "default"). out is always initialized;
 * valid=false when no band applies or parsing fails. */
hu_error_t hu_tapback_band_parse(hu_allocator_t *alloc, const char *json, size_t json_len,
                                 const char *contact_id, hu_tapback_band_t *out);

/* Read a bands file and parse it. Missing/unreadable file yields an
 * initialized out with valid=false (predicates then use the default cap). */
hu_error_t hu_tapback_band_load(hu_allocator_t *alloc, const char *path, const char *contact_id,
                                hu_tapback_band_t *out);

/* ~/.human/tapback_bands.json, or NULL in tests / when HOME is unset. */
const char *hu_tapback_bands_default_path(char *buf, size_t cap);

#endif /* HU_BEHAVIOR_TAPBACK_BAND_H */
