#ifndef HU_UPDATE_H
#define HU_UPDATE_H

#include "human/core/error.h"
#include <stddef.h>

/* Self-update mechanism. Stub. */
hu_error_t hu_update_check(char *version_buf, size_t buf_size);
hu_error_t hu_update_apply(void);

#endif /* HU_UPDATE_H */
