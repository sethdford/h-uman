#ifndef HU_OTEL_H
#define HU_OTEL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/observer.h"
#include <stddef.h>
#include <stdint.h>

typedef struct hu_otel_config {
    const char *endpoint;
    size_t endpoint_len;
    const char *service_name;
    size_t service_name_len;
    bool enable_traces;
    bool enable_metrics;
    bool enable_logs;
} hu_otel_config_t;

typedef struct hu_span hu_span_t;

hu_error_t hu_otel_observer_create(hu_allocator_t *alloc, const hu_otel_config_t *cfg,
                                   hu_observer_t *out);

hu_span_t *hu_span_start(hu_allocator_t *alloc, const char *name, size_t name_len);
void hu_span_set_attr_str(hu_span_t *span, const char *key, const char *value);
void hu_span_set_attr_int(hu_span_t *span, const char *key, int64_t value);
void hu_span_set_attr_double(hu_span_t *span, const char *key, double value);
hu_error_t hu_span_end(hu_span_t *span, hu_allocator_t *alloc);

#endif /* HU_OTEL_H */
