#ifndef SC_DOCTOR_H
#define SC_DOCTOR_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/config.h"
#include <stddef.h>

typedef enum sc_diag_severity {
    SC_DIAG_OK,
    SC_DIAG_WARN,
    SC_DIAG_ERR,
} sc_diag_severity_t;

typedef struct sc_diag_item {
    sc_diag_severity_t severity;
    const char *category;
    const char *message;
} sc_diag_item_t;

/* Parse df -m output; return available MB or 0 on parse fail. */
unsigned long sc_doctor_parse_df_available_mb(const char *df_output, size_t len);

/* Truncate string for display at valid UTF-8 boundary. Caller must free. */
sc_error_t sc_doctor_truncate_for_display(sc_allocator_t *alloc,
    const char *s, size_t len, size_t max_len, char **out);

/* Run config semantics checks; append items. */
sc_error_t sc_doctor_check_config_semantics(sc_allocator_t *alloc,
    const sc_config_t *cfg,
    sc_diag_item_t **items, size_t *count);

#endif /* SC_DOCTOR_H */
