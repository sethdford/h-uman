#ifndef HU_CALIBRATION_H
#define HU_CALIBRATION_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_personal_model;

typedef enum hu_calibration_tod_bucket {
    HU_CALIB_TOD_MORNING = 0,   /* local 06:00–12:00 */
    HU_CALIB_TOD_AFTERNOON = 1, /* 12:00–17:00 */
    HU_CALIB_TOD_EVENING = 2,   /* 17:00–22:00 */
    HU_CALIB_TOD_NIGHT = 3,     /* 22:00–06:00 */
    HU_CALIB_TOD_BUCKET_COUNT
} hu_calibration_tod_bucket_t;

typedef struct hu_calibration_latency_percentiles {
    double p25_sec;
    double p50_sec;
    double p75_sec;
    double p95_sec;
    uint32_t sample_count;
} hu_calibration_latency_percentiles_t;

typedef struct hu_calibration_contact_latency {
    char *handle_id;
    double median_reply_sec;
    uint32_t sample_count;
} hu_calibration_contact_latency_t;

typedef struct hu_timing_report {
    hu_calibration_latency_percentiles_t by_tod[HU_CALIB_TOD_BUCKET_COUNT];
    hu_calibration_contact_latency_t *contacts;
    size_t contacts_count;
    uint32_t active_hours[24];
    /* Percentile fields are message counts per calendar day (not seconds). */
    hu_calibration_latency_percentiles_t messages_per_day;
} hu_timing_report_t;

typedef struct hu_style_phrase_stat {
    char *phrase;
    uint32_t count;
} hu_style_phrase_stat_t;

typedef struct hu_style_report {
    double avg_message_length;
    double emoji_per_message;
    double exclamation_per_message;
    double question_per_message;
    hu_style_phrase_stat_t *opening_phrases;
    size_t opening_count;
    hu_style_phrase_stat_t *closing_phrases;
    size_t closing_count;
    double vocabulary_richness;
    uint32_t messages_analyzed;
} hu_style_report_t;

void hu_timing_report_deinit(hu_allocator_t *alloc, hu_timing_report_t *report);
void hu_style_report_deinit(hu_allocator_t *alloc, hu_style_report_t *report);

hu_error_t hu_calibration_analyze_timing(hu_allocator_t *alloc, const char *db_path,
                                         const char *contact_filter,
                                         hu_timing_report_t *out_report);

hu_error_t hu_calibration_analyze_style(hu_allocator_t *alloc, const char *db_path,
                                        const char *contact_filter, hu_style_report_t *out_report);

/* Caller frees *out_recommendations with hu_str_free(alloc, *out_recommendations).
 * channel_name: overlay channel key for recommendations (e.g. "imessage", "telegram").
 * NULL means auto-detected / unspecified ("auto" in JSON). */
hu_error_t hu_calibrate(hu_allocator_t *alloc, const char *db_path, const char *contact_filter,
                        const char *channel_name, char **out_recommendations);

/* ──────────────────────────────────────────────────────────────────────────
 * Reaction signature — summarizes the reactor-pattern facts ingested via
 * the iMessage / Slack / Discord reaction pipelines so the calibrated
 * persona can adapt per-contact. Facts whose `source_hint == "reaction_ingest"`
 * are aggregated by subject (contact handle), classified by predicate
 * (positive vs negative valence), and the most-common topic tokens
 * appearing in object strings are surfaced as "salient topics".
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_CALIB_REACTION_HANDLE_MAX   128
#define HU_CALIB_REACTION_TOP_REACTORS 8
#define HU_CALIB_REACTION_TOPIC_MAX    64
#define HU_CALIB_REACTION_TOPICS       16

typedef struct hu_calib_top_reactor {
    char handle[HU_CALIB_REACTION_HANDLE_MAX];
    uint32_t positive_count; /* love + like + laugh + emphasize + emoji */
    uint32_t negative_count; /* dislike + question */
    int64_t last_observed;
} hu_calib_top_reactor_t;

typedef struct hu_calib_reaction_signature {
    hu_calib_top_reactor_t top_reactors[HU_CALIB_REACTION_TOP_REACTORS];
    size_t reactor_count;
    char salient_topics[HU_CALIB_REACTION_TOPICS][HU_CALIB_REACTION_TOPIC_MAX];
    size_t salient_topic_count;
} hu_calib_reaction_signature_t;

/* Compute reaction signature from the personal model's facts array.
 * Scans facts where source_hint == "reaction_ingest", aggregates by
 * subject (contact), classifies by predicate (positive vs negative),
 * and extracts the most-common topic tokens from object strings.
 *
 * Returns the number of reactors populated (0 on NULL inputs). On
 * non-NULL `out` the struct is zero-initialized before the walk, so a
 * caller may inspect both `reactor_count` and `salient_topic_count`. */
size_t hu_calib_reaction_signature_from_model(const struct hu_personal_model *model,
                                              hu_calib_reaction_signature_t *out);

/* Append a top-level `"reactions":{...}` JSON object to `buf`, derived
 * from `sig`. The caller is responsible for appending a leading comma
 * (or other separator) if needed; this helper only emits the key/value
 * pair. Returns HU_OK on success, error from the JSON buf on failure.
 *
 * Always emits both inner keys (`top_reactors` and `salient_topics`) as
 * arrays — empty when the signature has none — so downstream parsers
 * can rely on a stable shape. */
hu_error_t hu_calib_reactions_append_json(hu_json_buf_t *buf,
                                          const hu_calib_reaction_signature_t *sig);

/* Variant of `hu_calibrate` that additionally embeds a `"reactions"`
 * object derived from `model`. When `model` is NULL or contains no
 * reaction-derived facts, the output matches `hu_calibrate` byte-for-
 * byte (no `"reactions"` key appears). Ownership / freeing rules match
 * the base call. */
hu_error_t hu_calibrate_with_model(hu_allocator_t *alloc, const char *db_path,
                                   const char *contact_filter, const char *channel_name,
                                   const struct hu_personal_model *model,
                                   char **out_recommendations);

#endif /* HU_CALIBRATION_H */
