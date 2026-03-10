#ifndef HU_DOCTOR_H
#define HU_DOCTOR_H

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

typedef enum hu_diag_severity {
    HU_DIAG_OK,
    HU_DIAG_WARN,
    HU_DIAG_ERR,
} hu_diag_severity_t;

typedef struct hu_diag_item {
    hu_diag_severity_t severity;
    const char *category;
    const char *message;
} hu_diag_item_t;

/* Parse df -m output; return available MB or 0 on parse fail. */
unsigned long hu_doctor_parse_df_available_mb(const char *df_output, size_t len);

/* Truncate string for display at valid UTF-8 boundary. Caller must free. */
hu_error_t hu_doctor_truncate_for_display(hu_allocator_t *alloc, const char *s, size_t len,
                                          size_t max_len, char **out);

/* Run config semantics checks; append items. */
hu_error_t hu_doctor_check_config_semantics(hu_allocator_t *alloc, const hu_config_t *cfg,
                                            hu_diag_item_t **items, size_t *count);

#endif /* HU_DOCTOR_H */
