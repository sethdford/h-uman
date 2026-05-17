/* include/human/ml/cli_kto.h — Phase 3 Task 9
 *
 * CLI handler for `human ml kto-train`. Trains a KTO trainer on
 * one-sided preference signals (desirable or undesirable).
 */
#ifndef HU_ML_CLI_KTO_H
#define HU_ML_CLI_KTO_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_ml_cli_kto_train(hu_allocator_t *alloc, int argc, const char **argv);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_CLI_KTO_H */
