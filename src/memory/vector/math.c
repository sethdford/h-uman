#include "seaclaw/memory/vector_math.h"
#include <string.h>
#include <math.h>

float sc_vector_cosine_similarity(const float *a, const float *b, size_t len) {
    if (!a || !b || len == 0) return 0.0f;

    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    for (size_t i = 0; i < len; i++) {
        double x = (double)a[i];
        double y = (double)b[i];
        if (x != x || y != y) return 0.0f; /* NaN */
        if (x > 1e38 || x < -1e38 || y > 1e38 || y < -1e38) return 0.0f; /* inf */
        dot += x * y;
        norm_a += x * x;
        norm_b += y * y;
    }

    double denom = sqrt(norm_a) * sqrt(norm_b);
    if (!isfinite(denom) || denom < 1e-300) return 0.0f;

    double raw = dot / denom;
    if (!isfinite(raw)) return 0.0f;
    if (raw < 0.0) raw = 0.0;
    if (raw > 1.0) raw = 1.0;
    return (float)raw;
}

unsigned char *sc_vector_to_bytes(sc_allocator_t *alloc, const float *v, size_t len) {
    if (!alloc || !v) return NULL;
    size_t byte_len = len * 4;
    unsigned char *bytes = (unsigned char *)alloc->alloc(alloc->ctx, byte_len);
    if (!bytes) return NULL;
    for (size_t i = 0; i < len; i++) {
        float f = v[i];
        unsigned char *p = bytes + i * 4;
        memcpy(p, &f, 4);
    }
    return bytes;
}

float *sc_vector_from_bytes(sc_allocator_t *alloc, const unsigned char *bytes, size_t byte_len) {
    if (!alloc || !bytes) return NULL;
    size_t count = byte_len / 4;
    if (count == 0) return NULL;
    float *v = (float *)alloc->alloc(alloc->ctx, count * sizeof(float));
    if (!v) return NULL;
    for (size_t i = 0; i < count; i++) {
        memcpy(&v[i], bytes + i * 4, 4);
    }
    return v;
}
