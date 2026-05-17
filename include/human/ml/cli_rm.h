/* include/human/ml/cli_rm.h — Phase 3 Task 10
 *
 * CLI handler for `human ml rm-train`. Trains a reward model using
 * Bradley-Terry loss on two-sided preference pairs. One-sided KTO
 * pairs are skipped with a log message.
 */
#ifndef HU_ML_CLI_RM_H
#define HU_ML_CLI_RM_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_ml_cli_rm_train(hu_allocator_t *alloc, int argc, const char **argv);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_CLI_RM_H */
