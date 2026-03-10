#ifndef HU_CHANNELS_CLI_H
#define HU_CHANNELS_CLI_H

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

hu_error_t hu_cli_create(hu_allocator_t *alloc, hu_channel_t *out);
void hu_cli_destroy(hu_channel_t *ch);

/* Read a line from stdin. Caller must free. Returns NULL on EOF. */
char *hu_cli_readline(hu_allocator_t *alloc, size_t *out_len);

/* True for exit, quit, :q */
bool hu_cli_is_quit_command(const char *line, size_t len);

#endif /* HU_CHANNELS_CLI_H */
