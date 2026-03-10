#ifndef HU_TOOLS_CLI_WRAPPER_COMMON_H
#define HU_TOOLS_CLI_WRAPPER_COMMON_H
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>

/* Returns false if the command contains dangerous characters */
bool hu_cli_sanitize_command(const char *cmd);

/* Split a command string into argv array. Returns number of args. */
size_t hu_cli_split_args(char *cmd, const char **argv_out, size_t max_out);

#endif
