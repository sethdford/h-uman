#ifndef SC_CLI_COMMANDS_H
#define SC_CLI_COMMANDS_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"

sc_error_t cmd_channel(sc_allocator_t *alloc, int argc, char **argv);
sc_error_t cmd_hardware(sc_allocator_t *alloc, int argc, char **argv);
sc_error_t cmd_memory(sc_allocator_t *alloc, int argc, char **argv);
sc_error_t cmd_workspace(sc_allocator_t *alloc, int argc, char **argv);
sc_error_t cmd_capabilities(sc_allocator_t *alloc, int argc, char **argv);
sc_error_t cmd_models(sc_allocator_t *alloc, int argc, char **argv);
sc_error_t cmd_auth(sc_allocator_t *alloc, int argc, char **argv);
sc_error_t cmd_update(sc_allocator_t *alloc, int argc, char **argv);
sc_error_t cmd_sandbox(sc_allocator_t *alloc, int argc, char **argv);

#endif /* SC_CLI_COMMANDS_H */
