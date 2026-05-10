#ifndef HU_AGENT_SCHEDULER_STATUS_JSON_H
#define HU_AGENT_SCHEDULER_STATUS_JSON_H

#include "human/core/error.h"
#include <stddef.h>

/* Parse ~/.human/scheduler.status JSON body. Keys may appear in any order;
 * whitespace after ':' is ignored. `on_ac_power_text` receives "true" or "false"
 * (NUL-terminated). All pointer parameters must be non-NULL; `on_ac_power_cap`
 * must be at least 6. Returns HU_OK only when all five fields parse cleanly;
 * HU_ERR_INVALID_ARGUMENT on bad arguments or incomplete/invalid JSON. */
hu_error_t hu_scheduler_status_parse_json(const char *json, unsigned long long *jobs_pending,
                                            unsigned long long *jobs_completed_today,
                                            long long *battery_pct,
                                            char *on_ac_power_text, size_t on_ac_power_cap,
                                            long long *updated_epoch);

#endif /* HU_AGENT_SCHEDULER_STATUS_JSON_H */
