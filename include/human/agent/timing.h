#ifndef HU_AGENT_TIMING_H
#define HU_AGENT_TIMING_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_timing_bucket {
    double p10;
    double p25;
    double p50;
    double p75;
    double p90;
    double mean;
    uint32_t sample_count;
} hu_timing_bucket_t;

typedef struct hu_timing_model {
    char *contact_id;
    size_t contact_id_len;
    hu_timing_bucket_t by_hour[24];
    hu_timing_bucket_t by_day[7];
    hu_timing_bucket_t by_msg_length[3]; /* 0=short <20, 1=medium 20-100, 2=long >100 */
    hu_timing_bucket_t overall;
    uint64_t computed_at;
} hu_timing_model_t;

hu_error_t hu_timing_model_build_query(const char *contact_id, size_t contact_id_len, char *buf,
                                       size_t cap, size_t *out_len);

hu_error_t hu_timing_bucket_from_samples(const double *samples, size_t count,
                                         hu_timing_bucket_t *out);

uint64_t hu_timing_model_sample(const hu_timing_model_t *model, uint8_t hour, uint8_t day_of_week,
                                size_t incoming_msg_len, uint32_t seed);

uint64_t hu_timing_model_sample_default(uint8_t hour, uint32_t seed);

uint64_t hu_timing_adjust(uint64_t base_delay_ms, uint8_t conversation_depth,
                          double emotional_intensity, bool calendar_busy,
                          uint64_t their_last_response_ms);

void hu_timing_model_deinit(hu_allocator_t *alloc, hu_timing_model_t *model);

#endif
