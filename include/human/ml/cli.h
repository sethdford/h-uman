#ifndef HU_ML_CLI_H
#define HU_ML_CLI_H

#include "human/core/allocator.h"
#include "human/core/error.h"

hu_error_t hu_ml_cli_train(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_experiment(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_prepare(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_status(hu_allocator_t *alloc, int argc, const char **argv);

#endif
