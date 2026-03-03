#ifndef SC_SERVICE_H
#define SC_SERVICE_H

#include "seaclaw/channel_loop.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>

void sc_service_configure(sc_channel_loop_ctx_t *ctx, sc_channel_loop_state_t *state);
sc_error_t sc_service_start(void);
void sc_service_stop(void);
bool sc_service_status(void);

#endif /* SC_SERVICE_H */
