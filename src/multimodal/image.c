#include "human/multimodal/image.h"
#include "human/core/string.h"
#include <ctype.h>
#include <string.h>

static int to_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool match_ext(const char *filename, size_t filename_len, const char *ext, size_t ext_len) {
    if (filename_len < ext_len + 1)
        return false;
    if (filename[filename_len - ext_len - 1] != '.')
        return false;
    for (size_t i = 0; i < ext_len; i++) {
        if (to_lower((unsigned char)filename[filename_len - ext_len + i]) != (unsigned char)ext[i])
            return false;
    }
    return true;
}

hu_image_format_t hu_image_detect_format(const char *filename, size_t filename_len) {
    if (!filename || filename_len == 0)
        return HU_IMAGE_UNKNOWN_FORMAT;
    if (match_ext(filename, filename_len, "jpg", 3) || match_ext(filename, filename_len, "jpeg", 4))
        return HU_IMAGE_JPEG;
    if (match_ext(filename, filename_len, "png", 3))
        return HU_IMAGE_PNG;
    if (match_ext(filename, filename_len, "gif", 3))
        return HU_IMAGE_GIF;
    if (match_ext(filename, filename_len, "webp", 4))
        return HU_IMAGE_WEBP;
    return HU_IMAGE_UNKNOWN_FORMAT;
}

const char *hu_image_mime_type(hu_image_format_t fmt) {
    switch (fmt) {
    case HU_IMAGE_JPEG:
        return "image/jpeg";
    case HU_IMAGE_PNG:
        return "image/png";
    case HU_IMAGE_GIF:
        return "image/gif";
    case HU_IMAGE_WEBP:
        return "image/webp";
    default:
        return "application/octet-stream";
    }
}

hu_error_t hu_image_build_vision_prompt(hu_allocator_t *alloc, const char *context,
                                        size_t context_len, hu_image_format_t format, char **out,
                                        size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *mime = hu_image_mime_type(format);
    const char *ctx = context && context_len > 0 ? context : "(none)";
    size_t ctx_len = context && context_len > 0 ? context_len : 6;

    char *prompt = hu_sprintf(alloc,
                              "Describe this image. Context: %.*s. Format: %s. Provide a "
                              "concise description suitable for conversation.",
                              (int)ctx_len, ctx, mime);
    if (!prompt)
        return HU_ERR_OUT_OF_MEMORY;
    *out = prompt;
    *out_len = strlen(prompt);
    return HU_OK;
}

static unsigned short read_be16(const unsigned char *p) {
    return (unsigned short)((p[0] << 8) | p[1]);
}

static unsigned int read_be32(const unsigned char *p) {
    return (unsigned int)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

static unsigned short read_le16(const unsigned char *p) {
    return (unsigned short)(p[0] | (p[1] << 8));
}

hu_error_t hu_image_parse_header(const unsigned char *data, size_t data_len,
                                 hu_image_metadata_t *out) {
    if (!data || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->format = HU_IMAGE_UNKNOWN_FORMAT;
    out->width = 0;
    out->height = 0;
    out->file_size = 0;
    out->mime_type = NULL;
    out->mime_type_len = 0;

    if (data_len < 8)
        return HU_ERR_PARSE;

    /* PNG: 89 50 4E 47 0D 0A 1A 0A */
    if (data_len >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
        out->format = HU_IMAGE_PNG;
        if (data_len >= 24) {
            out->width = read_be32(data + 16);
            out->height = read_be32(data + 20);
        }
        return HU_OK;
    }

    /* JPEG: FF D8 FF */
    if (data_len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        out->format = HU_IMAGE_JPEG;
        for (size_t i = 2; i + 9 <= data_len;) {
            if (data[i] != 0xFF) {
                i++;
                continue;
            }
            unsigned char marker = data[i + 1];
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                out->height = read_be16(data + i + 5);
                out->width = read_be16(data + i + 7);
                break;
            }
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9))
                i += 2;
            else if (i + 4 <= data_len) {
                unsigned short len = read_be16(data + i + 2);
                i += 2 + (size_t)len;
            } else
                break;
        }
        return HU_OK;
    }

    /* GIF: GIF87a or GIF89a */
    if (data_len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
        (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
        out->format = HU_IMAGE_GIF;
        if (data_len >= 10) {
            out->width = read_le16(data + 6);
            out->height = read_le16(data + 8);
        }
        return HU_OK;
    }

    /* WEBP: RIFF....WEBP */
    if (data_len >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        out->format = HU_IMAGE_WEBP;
        if (data_len >= 30) {
            out->width = read_le16(data + 26);
            out->height = read_le16(data + 28);
        }
        return HU_OK;
    }

    return HU_ERR_PARSE;
}

void hu_image_metadata_deinit(hu_image_metadata_t *meta, hu_allocator_t *alloc) {
    if (!meta || !alloc)
        return;
    if (meta->mime_type) {
        alloc->free(alloc->ctx, meta->mime_type, meta->mime_type_len + 1);
        meta->mime_type = NULL;
        meta->mime_type_len = 0;
    }
}
