#ifndef SC_SUPERHUMAN_PREDICTIVE_H
#define SC_SUPERHUMAN_PREDICTIVE_H

#include "seaclaw/agent/pattern_radar.h"
#include "seaclaw/agent/superhuman.h"
#include "seaclaw/core/error.h"

sc_error_t sc_superhuman_predictive_service(sc_pattern_radar_t *radar,
                                             sc_superhuman_service_t *out);

#endif /* SC_SUPERHUMAN_PREDICTIVE_H */
