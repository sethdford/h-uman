#ifndef HU_MEMORY_VECTOR_MATH_H
#define HU_MEMORY_VECTOR_MATH_H

#include "human/core/allocator.h"
#include <stddef.h>

/** Cosine similarity between two f32 vectors. Returns 0.0-1.0. */
float hu_vector_cosine_similarity(const float *a, const float *b, size_t len);

/** Serialize f32 vector to little-endian bytes. Caller frees with alloc->free. */
unsigned char *hu_vector_to_bytes(hu_allocator_t *alloc, const float *v, size_t len);

/** Deserialize bytes to f32 vector. Caller frees with alloc->free. */
float *hu_vector_from_bytes(hu_allocator_t *alloc, const unsigned char *bytes, size_t byte_len);

#endif
