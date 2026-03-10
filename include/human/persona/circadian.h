#ifndef HU_CIRCADIAN_H
#define HU_CIRCADIAN_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

typedef enum hu_time_phase {
    HU_PHASE_EARLY_MORNING, /* 5:00-8:00 */
    HU_PHASE_MORNING,       /* 8:00-12:00 */
    HU_PHASE_AFTERNOON,     /* 12:00-17:00 */
    HU_PHASE_EVENING,       /* 17:00-21:00 */
    HU_PHASE_NIGHT,         /* 21:00-0:00 */
    HU_PHASE_LATE_NIGHT,    /* 0:00-5:00 */
} hu_time_phase_t;

hu_time_phase_t hu_circadian_phase(uint8_t hour);
hu_error_t hu_circadian_build_prompt(hu_allocator_t *alloc, uint8_t hour,
                                      char **out, size_t *out_len);

#endif
