#ifndef SC_TOOLS_CLI_WRAPPER_COMMON_H
#define SC_TOOLS_CLI_WRAPPER_COMMON_H
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stdbool.h>
#include <stddef.h>

/* Returns false if the command contains dangerous characters */
bool sc_cli_sanitize_command(const char *cmd);

/* Split a command string into argv array. Returns number of args. */
size_t sc_cli_split_args(char *cmd, const char **argv_out, size_t max_out);

#endif
