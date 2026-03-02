#ifndef SC_CHANNELS_CLI_H
#define SC_CHANNELS_CLI_H

#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

sc_error_t sc_cli_create(sc_allocator_t *alloc, sc_channel_t *out);
void sc_cli_destroy(sc_channel_t *ch);

/* Read a line from stdin. Caller must free. Returns NULL on EOF. */
char *sc_cli_readline(sc_allocator_t *alloc, size_t *out_len);

/* True for exit, quit, :q */
bool sc_cli_is_quit_command(const char *line, size_t len);

#endif /* SC_CHANNELS_CLI_H */
