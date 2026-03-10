#ifndef HU_MULTIMODAL_H
#define HU_MULTIMODAL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_MULTIMODAL_MAX_IMAGE_SIZE (5 * 1024 * 1024)

typedef struct hu_multimodal_config {
    uint32_t max_images;
    size_t max_image_size_bytes;
    bool allow_remote_fetch;
    const char *const *allowed_dirs;
    size_t allowed_dirs_count;
} hu_multimodal_config_t;

typedef enum hu_image_ref_type {
    HU_IMAGE_REF_LOCAL,
    HU_IMAGE_REF_URL,
    HU_IMAGE_REF_DATA_URI,
} hu_image_ref_type_t;

typedef struct hu_image_ref {
    hu_image_ref_type_t type;
    const char *value;
    size_t value_len;
} hu_image_ref_t;

/* Base64 encode raw bytes */
hu_error_t hu_multimodal_encode_base64(hu_allocator_t *alloc, const void *data, size_t data_len,
                                       char **out_base64, size_t *out_len);

/* Detect MIME type from first few bytes */
const char *hu_multimodal_detect_mime(const void *header, size_t header_len);

/* Detect audio MIME type from file path extension (e.g. .wav -> "audio/wav") */
const char *hu_multimodal_detect_audio_mime(const char *path, size_t path_len);

/* Encode a local image file to base64 data URI */
hu_error_t hu_multimodal_encode_image(hu_allocator_t *alloc, const char *file_path,
                                      char **out_data_uri, size_t *out_len);

/* Legacy: encode raw image bytes to base64 (uses system allocator). */
hu_error_t hu_multimodal_encode_image_raw(const void *img_data, size_t len, char **out_base64);

/* Parse [IMAGE:...] markers from text */
hu_error_t hu_multimodal_parse_markers(hu_allocator_t *alloc, const char *text, size_t text_len,
                                       hu_image_ref_t **out_refs, size_t *out_ref_count,
                                       char **out_cleaned_text, size_t *out_cleaned_len);

/* Build provider-specific image content JSON */
hu_error_t hu_multimodal_build_openai_image(hu_allocator_t *alloc, const char *data_uri,
                                            size_t data_uri_len, char **out_json,
                                            size_t *out_json_len);

hu_error_t hu_multimodal_build_anthropic_image(hu_allocator_t *alloc, const char *mime_type,
                                               const char *base64_data, size_t base64_len,
                                               char **out_json, size_t *out_json_len);

hu_error_t hu_multimodal_build_gemini_image(hu_allocator_t *alloc, const char *mime_type,
                                            const char *base64_data, size_t base64_len,
                                            char **out_json, size_t *out_json_len);

#endif /* HU_MULTIMODAL_H */
