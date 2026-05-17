#ifndef HU_ML_CLI_DEMO_H
#define HU_ML_CLI_DEMO_H

#include "human/core/allocator.h"
#include "human/core/error.h"

/* Entrypoint for `human demo rl-closed-loop`.
 * Exit codes: 0 = pass, 2 = soft fail, 3 = harness error. */
hu_error_t hu_ml_cli_demo_rl_closed_loop(int argc, const char **argv, hu_allocator_t *alloc);

#endif
