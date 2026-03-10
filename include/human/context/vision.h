#ifndef HU_CONTEXT_VISION_H
#define HU_CONTEXT_VISION_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>

/* Read an image file and encode it as base64 for the provider.
 * Supports JPEG, PNG, GIF, WEBP.
 * Returns the base64-encoded data and the media type string.
 * Caller owns both returned strings. */
hu_error_t hu_vision_read_image(hu_allocator_t *alloc, const char *path, size_t path_len,
                                char **base64_out, size_t *base64_len,
                                char **media_type_out, size_t *media_type_len);

/* Build a vision request to describe an image.
 * Creates a chat message with the image as a content part and a text prompt
 * asking for a brief description.
 * Caller owns the returned description string. */
hu_error_t hu_vision_describe_image(hu_allocator_t *alloc, hu_provider_t *provider,
                                    const char *image_path, size_t image_path_len,
                                    const char *model, size_t model_len,
                                    char **description_out, size_t *description_len);

/* Build context for the prompt when an image description is available.
 * Returns a context string like:
 * "The user shared an image: [description]. Respond naturally to what you see."
 * Caller owns returned string. Returns NULL if description is NULL. */
char *hu_vision_build_context(hu_allocator_t *alloc, const char *description,
                              size_t description_len, size_t *out_len);

#endif /* HU_CONTEXT_VISION_H */
