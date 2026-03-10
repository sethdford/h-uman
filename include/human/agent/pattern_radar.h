#ifndef HU_PATTERN_RADAR_H
#define HU_PATTERN_RADAR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define HU_PATTERN_MAX_OBSERVATIONS 100

typedef enum hu_pattern_type {
    HU_PATTERN_TOPIC_RECURRENCE,
    HU_PATTERN_EMOTIONAL_TREND,
    HU_PATTERN_BEHAVIORAL_CYCLE,
    HU_PATTERN_RELATIONSHIP_SHIFT,
    HU_PATTERN_GROWTH,
} hu_pattern_type_t;

typedef struct hu_pattern_observation {
    hu_pattern_type_t type;
    char *subject;
    size_t subject_len;
    char *detail;
    size_t detail_len;
    uint32_t occurrence_count;
    double confidence;
    char *first_seen;
    char *last_seen;
} hu_pattern_observation_t;

typedef struct hu_pattern_radar {
    hu_allocator_t alloc;
    hu_pattern_observation_t observations[HU_PATTERN_MAX_OBSERVATIONS];
    size_t observation_count;
} hu_pattern_radar_t;

hu_error_t hu_pattern_radar_init(hu_pattern_radar_t *radar, hu_allocator_t alloc);
hu_error_t hu_pattern_radar_observe(hu_pattern_radar_t *radar,
                                     const char *subject, size_t subject_len,
                                     hu_pattern_type_t type,
                                     const char *detail, size_t detail_len,
                                     const char *timestamp, size_t timestamp_len);
hu_error_t hu_pattern_radar_build_context(hu_pattern_radar_t *radar, hu_allocator_t *alloc,
                                           char **out, size_t *out_len);
void hu_pattern_radar_deinit(hu_pattern_radar_t *radar);

#endif
