#ifndef HU_TOOLS_IMAGE_GEN_H
#define HU_TOOLS_IMAGE_GEN_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"

hu_error_t hu_image_gen_create(hu_allocator_t *alloc, hu_tool_t *out);

/* JSON body for POST /v1/images/generations with gpt-image-1. DALL·E was
 * shut down 2026-05-12; gpt-image takes no response_format (it always answers
 * with base64), quality low|medium|high (DALL·E "hd" -> high, "standard" or
 * NULL -> medium) and size 1024x1024|1536x1024|1024x1536 (DALL·E 1792 sizes
 * map to the nearest; NULL -> 1024x1024). *out_body is alloc-owned (len+1). */
hu_error_t hu_image_gen_build_request(hu_allocator_t *alloc, const char *prompt, size_t prompt_len,
                                      const char *size, const char *quality, char **out_body,
                                      size_t *out_len);

/* Decode data[0].b64_json from an images-API response into bytes (alloc-owned,
 * free with out_len). HU_ERR_IO when the response carries no image. */
hu_error_t hu_image_gen_parse_image(hu_allocator_t *alloc, const char *body, size_t body_len,
                                    void **out_bytes, size_t *out_len);

/* Same HTTPS path as the image_generate tool (default size/quality); copies URL into @p out_url.
 * HU_OK on success. HU_ERR_IO if generation failed (no key, HTTP error, parse error). */
hu_error_t hu_image_gen_url_into_buffer(hu_allocator_t *alloc, const char *query, size_t query_len,
                                        char *out_url, size_t out_url_cap);

/** Generate an image and download it to a temp file. Caller must unlink() the file.
 *  Writes the temp path into @p out_path (capacity @p out_path_cap).
 *  HU_OK on success. HU_ERR_IO on failure. */
hu_error_t hu_image_gen_download(hu_allocator_t *alloc, const char *prompt, size_t prompt_len,
                                 char *out_path, size_t out_path_cap);

#endif /* HU_TOOLS_IMAGE_GEN_H */
