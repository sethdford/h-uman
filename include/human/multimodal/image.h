#ifndef HU_MULTIMODAL_IMAGE_H
#define HU_MULTIMODAL_IMAGE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum hu_image_format {
    HU_IMAGE_JPEG,
    HU_IMAGE_PNG,
    HU_IMAGE_GIF,
    HU_IMAGE_WEBP,
    HU_IMAGE_UNKNOWN_FORMAT,
} hu_image_format_t;

typedef struct hu_image_metadata {
    hu_image_format_t format;
    size_t width;
    size_t height;
    size_t file_size;
    char *mime_type;
    size_t mime_type_len;
} hu_image_metadata_t;

/* Detect image format from file extension */
hu_image_format_t hu_image_detect_format(const char *filename, size_t filename_len);

/* Get MIME type string for a format */
const char *hu_image_mime_type(hu_image_format_t fmt);

/* Build a vision prompt for image description */
hu_error_t hu_image_build_vision_prompt(hu_allocator_t *alloc, const char *context,
                                        size_t context_len, hu_image_format_t format, char **out,
                                        size_t *out_len);

/* Parse image metadata from file header bytes (first 32 bytes) */
hu_error_t hu_image_parse_header(const unsigned char *data, size_t data_len,
                                 hu_image_metadata_t *out);

/* Free metadata strings */
void hu_image_metadata_deinit(hu_image_metadata_t *meta, hu_allocator_t *alloc);

#endif
