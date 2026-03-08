#ifndef SC_MULTIMODAL_IMAGE_H
#define SC_MULTIMODAL_IMAGE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum sc_image_format {
    SC_IMAGE_JPEG,
    SC_IMAGE_PNG,
    SC_IMAGE_GIF,
    SC_IMAGE_WEBP,
    SC_IMAGE_UNKNOWN_FORMAT,
} sc_image_format_t;

typedef struct sc_image_metadata {
    sc_image_format_t format;
    size_t width;
    size_t height;
    size_t file_size;
    char *mime_type;
    size_t mime_type_len;
} sc_image_metadata_t;

/* Detect image format from file extension */
sc_image_format_t sc_image_detect_format(const char *filename, size_t filename_len);

/* Get MIME type string for a format */
const char *sc_image_mime_type(sc_image_format_t fmt);

/* Build a vision prompt for image description */
sc_error_t sc_image_build_vision_prompt(sc_allocator_t *alloc, const char *context,
                                        size_t context_len, sc_image_format_t format, char **out,
                                        size_t *out_len);

/* Parse image metadata from file header bytes (first 32 bytes) */
sc_error_t sc_image_parse_header(const unsigned char *data, size_t data_len,
                                 sc_image_metadata_t *out);

/* Free metadata strings */
void sc_image_metadata_deinit(sc_image_metadata_t *meta, sc_allocator_t *alloc);

#endif
