#ifndef HU_SERVICE_H
#define HU_SERVICE_H

#include "human/channel_loop.h"
#include "human/core/error.h"
#include <stdbool.h>

void hu_service_configure(hu_channel_loop_ctx_t *ctx, hu_channel_loop_state_t *state);
hu_error_t hu_service_start(void);
void hu_service_stop(void);
bool hu_service_status(void);

#endif /* HU_SERVICE_H */
