#ifndef HU_CLI_COMMANDS_H
#define HU_CLI_COMMANDS_H

#include "human/core/allocator.h"
#include "human/core/error.h"

hu_error_t cmd_channel(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_hardware(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_memory(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_workspace(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_capabilities(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_models(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_auth(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_update(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_sandbox(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t cmd_init(hu_allocator_t *alloc, int argc, char **argv);

#endif /* HU_CLI_COMMANDS_H */
