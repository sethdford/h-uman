#ifndef SC_MULTIMODAL_H
#define SC_MULTIMODAL_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define SC_MULTIMODAL_MAX_IMAGE_SIZE (5 * 1024 * 1024)

typedef struct sc_multimodal_config {
    uint32_t max_images;
    size_t max_image_size_bytes;
    bool allow_remote_fetch;
    const char *const *allowed_dirs;
    size_t allowed_dirs_count;
} sc_multimodal_config_t;

typedef enum sc_image_ref_type {
    SC_IMAGE_REF_LOCAL,
    SC_IMAGE_REF_URL,
    SC_IMAGE_REF_DATA_URI,
} sc_image_ref_type_t;

typedef struct sc_image_ref {
    sc_image_ref_type_t type;
    const char *value;
    size_t value_len;
} sc_image_ref_t;

/* Base64 encode raw bytes */
sc_error_t sc_multimodal_encode_base64(sc_allocator_t *alloc,
    const void *data, size_t data_len,
    char **out_base64, size_t *out_len);

/* Detect MIME type from first few bytes */
const char *sc_multimodal_detect_mime(const void *header, size_t header_len);

/* Encode a local image file to base64 data URI */
sc_error_t sc_multimodal_encode_image(sc_allocator_t *alloc,
    const char *file_path,
    char **out_data_uri, size_t *out_len);

/* Legacy: encode raw image bytes to base64 (uses system allocator). */
sc_error_t sc_multimodal_encode_image_raw(const void *img_data, size_t len, char **out_base64);

/* Parse [IMAGE:...] markers from text */
sc_error_t sc_multimodal_parse_markers(sc_allocator_t *alloc,
    const char *text, size_t text_len,
    sc_image_ref_t **out_refs, size_t *out_ref_count,
    char **out_cleaned_text, size_t *out_cleaned_len);

/* Build provider-specific image content JSON */
sc_error_t sc_multimodal_build_openai_image(sc_allocator_t *alloc,
    const char *data_uri, size_t data_uri_len,
    char **out_json, size_t *out_json_len);

sc_error_t sc_multimodal_build_anthropic_image(sc_allocator_t *alloc,
    const char *mime_type, const char *base64_data, size_t base64_len,
    char **out_json, size_t *out_json_len);

sc_error_t sc_multimodal_build_gemini_image(sc_allocator_t *alloc,
    const char *mime_type, const char *base64_data, size_t base64_len,
    char **out_json, size_t *out_json_len);

#endif /* SC_MULTIMODAL_H */
