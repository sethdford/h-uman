#ifndef HU_EVAL_CLI_EVAL_H
#define HU_EVAL_CLI_EVAL_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_eval_cli_competitive(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t hu_eval_cli_leaderboard(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t hu_eval_cli_gate(hu_allocator_t *alloc, int argc, char **argv);

#ifdef HU_IS_TEST
bool hu_build_has_competitive_eval(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_CLI_EVAL_H */
