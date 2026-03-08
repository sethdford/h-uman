#ifndef SC_PATTERN_RADAR_H
#define SC_PATTERN_RADAR_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define SC_PATTERN_MAX_OBSERVATIONS 100

typedef enum sc_pattern_type {
    SC_PATTERN_TOPIC_RECURRENCE,
    SC_PATTERN_EMOTIONAL_TREND,
    SC_PATTERN_BEHAVIORAL_CYCLE,
    SC_PATTERN_RELATIONSHIP_SHIFT,
    SC_PATTERN_GROWTH,
} sc_pattern_type_t;

typedef struct sc_pattern_observation {
    sc_pattern_type_t type;
    char *subject;
    size_t subject_len;
    char *detail;
    size_t detail_len;
    uint32_t occurrence_count;
    double confidence;
    char *first_seen;
    char *last_seen;
} sc_pattern_observation_t;

typedef struct sc_pattern_radar {
    sc_allocator_t alloc;
    sc_pattern_observation_t observations[SC_PATTERN_MAX_OBSERVATIONS];
    size_t observation_count;
} sc_pattern_radar_t;

sc_error_t sc_pattern_radar_init(sc_pattern_radar_t *radar, sc_allocator_t alloc);
sc_error_t sc_pattern_radar_observe(sc_pattern_radar_t *radar,
                                     const char *subject, size_t subject_len,
                                     sc_pattern_type_t type,
                                     const char *detail, size_t detail_len,
                                     const char *timestamp, size_t timestamp_len);
sc_error_t sc_pattern_radar_build_context(sc_pattern_radar_t *radar, sc_allocator_t *alloc,
                                           char **out, size_t *out_len);
void sc_pattern_radar_deinit(sc_pattern_radar_t *radar);

#endif
