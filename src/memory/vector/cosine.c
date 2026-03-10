#include "human/core/allocator.h"
#include "human/memory/vector.h"
#include <math.h>

void hu_embedding_free(hu_allocator_t *alloc, hu_embedding_t *e) {
    if (alloc && e && e->values && e->dim > 0) {
        alloc->free(alloc->ctx, e->values, e->dim * sizeof(float));
        e->values = NULL;
        e->dim = 0;
    }
}

void hu_vector_entries_free(hu_allocator_t *alloc, hu_vector_entry_t *entries, size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].id)
            alloc->free(alloc->ctx, (void *)entries[i].id, entries[i].id_len + 1);
        hu_embedding_free(alloc, (hu_embedding_t *)&entries[i].embedding);
        if (entries[i].content)
            alloc->free(alloc->ctx, (void *)entries[i].content, entries[i].content_len + 1);
    }
    alloc->free(alloc->ctx, entries, sizeof(hu_vector_entry_t) * count);
}

float hu_cosine_similarity(const float *a, const float *b, size_t dim) {
    if (!a || !b || dim == 0)
        return 0.0f;

    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;

    for (size_t i = 0; i < dim; i++) {
        double x = (double)a[i];
        double y = (double)b[i];
        dot += x * y;
        norm_a += x * x;
        norm_b += y * y;
    }

    double denom = sqrt(norm_a) * sqrt(norm_b);
    if (denom < 1e-300 || !isfinite(denom))
        return 0.0f;

    double raw = dot / denom;
    if (!isfinite(raw))
        return 0.0f;
    return (float)raw;
}
