#ifndef HU_FEEDS_EMAIL_H
#define HU_FEEDS_EMAIL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HU_ENABLE_FEEDS

typedef struct hu_email_header {
    char subject[256];
    char from[256];
    int64_t date;
} hu_email_header_t;

hu_error_t hu_email_fetch_unread(hu_allocator_t *alloc,
    hu_email_header_t *headers, size_t cap, size_t *out_count);

#endif /* HU_ENABLE_FEEDS */

#ifdef __cplusplus
}
#endif

#endif
