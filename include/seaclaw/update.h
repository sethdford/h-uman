#ifndef SC_UPDATE_H
#define SC_UPDATE_H

#include "seaclaw/core/error.h"
#include <stddef.h>

/* Self-update mechanism. Stub. */
sc_error_t sc_update_check(char *version_buf, size_t buf_size);
sc_error_t sc_update_apply(void);

#endif /* SC_UPDATE_H */
