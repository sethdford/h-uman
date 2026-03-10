#ifndef HU_SUPERHUMAN_PREDICTIVE_H
#define HU_SUPERHUMAN_PREDICTIVE_H

#include "human/agent/pattern_radar.h"
#include "human/agent/superhuman.h"
#include "human/core/error.h"

hu_error_t hu_superhuman_predictive_service(hu_pattern_radar_t *radar,
                                             hu_superhuman_service_t *out);

#endif /* HU_SUPERHUMAN_PREDICTIVE_H */
