#ifndef SC_OTEL_H
#define SC_OTEL_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/observer.h"
#include <stddef.h>
#include <stdint.h>

typedef struct sc_otel_config {
    const char *endpoint;
    size_t endpoint_len;
    const char *service_name;
    size_t service_name_len;
    bool enable_traces;
    bool enable_metrics;
    bool enable_logs;
} sc_otel_config_t;

typedef struct sc_span sc_span_t;

sc_error_t sc_otel_observer_create(sc_allocator_t *alloc, const sc_otel_config_t *cfg,
                                   sc_observer_t *out);

sc_span_t *sc_span_start(sc_allocator_t *alloc, const char *name, size_t name_len);
void sc_span_set_attr_str(sc_span_t *span, const char *key, const char *value);
void sc_span_set_attr_int(sc_span_t *span, const char *key, int64_t value);
void sc_span_set_attr_double(sc_span_t *span, const char *key, double value);
sc_error_t sc_span_end(sc_span_t *span, sc_allocator_t *alloc);

#endif /* SC_OTEL_H */
